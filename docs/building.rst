Building and Installing
=======================

.. note::

   rocm-ernic is early-access software that has been tested on
   limited hardware. It may not work on your system yet.

Requirements
------------

rocm-ernic builds with GCC or Clang on Linux, and is tested on
Ubuntu 24.04. It needs:

- CMake 3.21 or later
- pkg-config
- libvfio-user, built from source (see below), which needs Meson
  and Ninja to build
- libibverbs and librdmacm development packages
- GLib 2.0 development package
- json-c development package. libvfio-user needs it, and with
  ``ERNIC_BUILD_P4`` rocm-ernic links it too.

Install them on Ubuntu/Debian:

.. code-block:: bash

   sudo apt install cmake meson ninja-build pkg-config \
     libibverbs-dev librdmacm-dev libglib2.0-dev libjson-c-dev

Build and install ``libvfio-user`` if it is not already
available on your system:

.. code-block:: bash

   git clone https://github.com/nutanix/libvfio-user.git
   cd libvfio-user
   meson setup build --prefix=/usr
   ninja -C build
   sudo ninja -C build install
   sudo ldconfig

If libvfio-user is installed somewhere other than ``/usr`` or
``/usr/local``, add its ``pkgconfig`` directory to
``PKG_CONFIG_PATH``, or point CMake at the prefix with
``-DCMAKE_PREFIX_PATH``, e.g.
``-DCMAKE_PREFIX_PATH="$HOME/.local"``.

Compilation
-----------

From the project root directory:

.. code-block:: bash

   cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
   cmake --build build
   ctest --test-dir build

The executable is produced at ``build/rocm-ernic``. To check it,
start the server with the loopback backend:

.. code-block:: bash

   ./build/rocm-ernic \
     --socket /tmp/vfio-user-rocm-ernic.sock \
     --backend loopback --verbose

That presents the emulated ionic device (``1dd8:1002``,
subsystem ``1dd8:5400``) to any client that connects to the
socket.

Installation
------------

.. code-block:: bash

   sudo cmake --install build

By default the binary installs to ``/usr/local/bin/rocm-ernic``,
and its licenses to ``/usr/local/share/doc/rocm-ernic``; a
default install places nothing else.
Override the destination with ``-DCMAKE_INSTALL_PREFIX=<path>``.

Configure with ``-DERNIC_INSTALL_SERVICE=ON`` to additionally
install the systemd units, ``ernicctl``, the launcher, the
Prometheus exporter, and the ionic driver patches;
:doc:`service` lists the full set of installed files. These
files refer to each other by their installed paths, which come
from ``CMAKE_INSTALL_PREFIX`` when the project is configured, so
to install them somewhere else, configure with that prefix rather
than passing ``--prefix`` to ``cmake --install``. To stage an
install, use ``DESTDIR``.

.. _nightly-packages:

Nightly Packages
----------------

Every night that ``develop`` changes, DEB packages of it for
Ubuntu 24.04 and 26.04 are published as the
`nightly prerelease
<https://github.com/ROCm/rocm-ernic/releases/tag/nightly>`_.
They're development builds for testing, not supported releases.
Download the packages for your Ubuntu release (the file names end
with it) and install them:

.. code-block:: bash

   sudo apt install ./rocm-ernic_<version>_amd64.deb \
     ./rocm-ernic-exporter_<version>_all.deb
   sudo systemctl daemon-reload

``rocm-ernic`` has the server, with its own copy of
libvfio-user, the systemd service, ``ernicctl``, and the guest
driver pack, installed under ``/usr``. Its configuration is in
``/etc/rocm-ernic/rocm-ernic.env`` (see :doc:`service`).
``rocm-ernic-exporter`` has the Prometheus exporter, which also
needs ``prometheus_client`` (see :doc:`monitoring`).

.. _packages:

Packages
--------

CPack makes DEB packages of the installed files, one for each
install component:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Package
     - Contents
   * - ``rocm-ernic``
     - The server, and with ``ERNIC_INSTALL_SERVICE``, the
       systemd service, ``ernicctl``, and the guest driver pack.
       ``/etc/rocm-ernic/rocm-ernic.env`` is a configuration file,
       which upgrades don't replace.
   * - ``rocm-ernic-exporter``
     - The Prometheus exporter, its systemd unit, and the Grafana
       dashboard (``ERNIC_INSTALL_SERVICE`` only). It needs
       ``prometheus_client`` in a virtual environment; see
       :doc:`monitoring`.

Ubuntu doesn't package libvfio-user, and libvfio-user keeps the
soname ``libvfio-user.so.0`` through incompatible changes, so a
package can't rely on another copy of it. With
``-DERNIC_BUNDLE_VFIO_USER=ON``, the package has its own copy, in
``<libdir>/rocm-ernic``, which only the installed server loads:
the server finds it through a ``DT_RPATH`` relative to its own
location, which ``LD_LIBRARY_PATH`` can't override. The copy has
to be linked with full RELRO, like the server, which configuring
checks.

The nightly packages are made like this, installed under
``/usr``:

.. code-block:: bash

   # libvfio-user, built for the package
   cd libvfio-user
   LDFLAGS="-Wl,-z,relro,-z,now" meson setup build \
     --prefix="$HOME/vfu-pkg" --libdir=lib \
     --buildtype=debugoptimized -Ddebug-logs=disabled \
     -Dwarning_level=0
   meson compile -C build vfio-user
   meson install -C build --no-rebuild --tags runtime,devel

   # rocm-ernic
   cd ../rocm-ernic
   cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
     -DCMAKE_INSTALL_PREFIX=/usr \
     -DCMAKE_PREFIX_PATH="$HOME/vfu-pkg" \
     -DERNIC_INSTALL_SERVICE=ON \
     -DERNIC_BUNDLE_VFIO_USER=ON \
     -DERNIC_VFIO_USER_LICENSE="$PWD/../libvfio-user/LICENSE"
   cmake --build build
   cd build && cpack -G DEB

``cpack`` needs ``dpkg-shlibdeps``, from ``dpkg-dev``, and
``file`` to find the packages' dependencies. The nightly build
workflow, ``.github/workflows/nightly.yml``, publishes the
packages for each supported Ubuntu release as the ``nightly``
prerelease, once they've installed and run in a clean container.

The ``ernic-package-deb`` CTest test builds the packages of the
build's configuration and checks their files, file modes,
dependencies, configuration file, and the paths in the service
files, and, with ``ERNIC_BUNDLE_VFIO_USER``, the server's
``DT_RPATH``. It's registered in a top-level build when
``dpkg-dev``, ``file``, and ``readelf`` are installed. A change to
the install rules has to update its lists of files, in
``tests/package/run-package-test.cmake``. The
``ernic-package-deb-config-paths`` test, registered with it,
configures and builds the server again with prefixes that move
the configuration file, such as ``/opt/<name>``, and checks
where the packages install it and where the installed files
read it.

Build Options
-------------

.. list-table::
   :header-rows: 1
   :widths: 30 10 60

   * - Option
     - Default
     - Description
   * - ``CMAKE_BUILD_TYPE``
     - ``RelWithDebInfo``
     - Build type (Debug, Release, RelWithDebInfo, etc.).
       When rocm-ernic is a subproject, the parent project's
       build type is used instead. CI builds Debug and Release,
       so name one of them to match it
   * - ``BUILD_TESTING``
     - ``ON``
     - Build the tests and register them with CTest
   * - ``ERNIC_WERROR``
     - ``OFF``
     - Treat compiler warnings as errors for project code.
       Off by default so a build with an unfamiliar compiler
       is never broken by a new warning; CI turns it on
       (see :ref:`werror-policy`)
   * - ``ERNIC_TRIVIAL_AUTO_VAR_INIT``
     - ``ON``
     - Fill every local variable the code doesn't initialize
       with a pattern (``-ftrivial-auto-var-init=pattern``;
       GCC 12 or later, or Clang). Turn it off for a build
       run under valgrind, since it hides the reads of
       uninitialized locals that memcheck reports
   * - ``ERNIC_USE_SANITIZERS``
     - ``OFF``
     - Build with ASan, LSan, UBSan, and the compiler's other
       checks that can be combined with them
       (see :ref:`sanitizers`)
   * - ``ERNIC_USE_THREAD_SANITIZER``
     - ``OFF``
     - Build with TSan (mutually exclusive with above)
   * - ``ERNIC_SANITIZER_SHARED_RUNTIME``
     - ``ON`` (GCC), ``OFF`` (Clang)
     - Link the shared sanitizer runtime instead of the
       static one
   * - ``ERNIC_USE_CODE_COVERAGE``
     - ``OFF``
     - Build with LLVM source-based code coverage; Clang only
       (see :ref:`code-coverage`)
   * - ``ERNIC_USE_IWYU``
     - ``OFF``
     - Run include-what-you-use while compiling; advisory
       (see :ref:`iwyu`)
   * - ``ERNIC_USE_CLANG_TIDY``
     - ``OFF``
     - Run clang-tidy while compiling; every finding is an
       error. Clang only (see :ref:`clang-tidy`)
   * - ``ERNIC_BUILD_P4``
     - ``OFF``
     - Build the P4 match-action engine and its tests; needs
       json-c (see :doc:`p4`)
   * - ``ERNIC_BUILD_DOCS``
     - ``OFF``
     - Build Sphinx + Breathe + Doxygen documentation
   * - ``ERNIC_DOCS_ONLY``
     - ``OFF``
     - Configure only documentation targets (no library
       dependencies required)
   * - ``ERNIC_BUILD_KMOD``
     - ``OFF``
     - Enable the DKMS targets that build the patched
       upstream ionic guest modules (see :doc:`ionic`)
   * - ``IONIC_KERNEL_REF``
     - ``v7.2.6``
     - Linux kernel tag or SHA the ionic sources are fetched
       from; must be ``v6.18`` or newer
   * - ``IONIC_KERNEL_REPO``
     - kernel.org stable
     - Linux kernel git repository the ionic sources are
       fetched from
   * - ``ERNIC_INSTALL_SERVICE``
     - ``OFF``
     - Also install the systemd units, ``ernicctl``, the
       launcher, and the Prometheus exporter
       (see :doc:`service`)
   * - ``ERNIC_BUNDLE_VFIO_USER``
     - ``OFF``
     - Install a private copy of libvfio-user that only the
       installed server uses, for packages (see :ref:`packages`)
   * - ``ERNIC_VFIO_USER_LICENSE``
     - (none)
     - libvfio-user's ``LICENSE`` file, installed with the private
       copy; required by ``ERNIC_BUNDLE_VFIO_USER``
   * - ``ERNIC_PACKAGE_RELEASE``
     - ``local``
     - The release part of the DEB packages' version, after the
       project's version (see :ref:`packages`): letters, digits,
       ``+``, ``.``, and ``~``, but not a value CMake takes as
       false, such as ``0``, which CPack would leave out
   * - ``CMAKE_INSTALL_PREFIX``
     - ``/usr/local``
     - Installation prefix

.. _werror-policy:

Warnings as Errors
------------------

``ERNIC_WERROR`` adds ``-Werror`` to every target that
``ernic_set_compiler_flags()`` sets up: the server, the test
programs, and the fuzz harness replay programs.

It defaults to ``OFF`` so that a packager or downstream
consumer building with a compiler version we have not tested
is never blocked by a newly-introduced warning. That
tolerance is not extended to our own CI: the jobs that build
the project to test or release it configure with
``-DERNIC_WERROR=ON``, so a new warning fails the build
before it can merge.

Developers should build with it on, matching CI:

.. code-block:: bash

   cmake -B build -G Ninja -DERNIC_WERROR=ON

Some CI builds leave it off. The CodeQL, clang-tidy, and
include-what-you-use jobs build the project only to analyze
it, and leave checking for warnings to the Build and Test
workflow. The ``nix`` static-analysis, dynamic-analysis, and
fuzz builds leave it off because gcc 15 and current clang
emit warnings on the QEMU-ported headers that these builds
cannot suppress. Those lanes are informational and do not
gate merges.

.. _sanitizers:

Sanitizers
----------

``ERNIC_USE_SANITIZERS`` builds the server and the test programs
with every sanitizer except ThreadSanitizer that the compiler
supports and that can be combined with AddressSanitizer:

- With GCC and Clang: AddressSanitizer (including
  LeakSanitizer), UndefinedBehaviorSanitizer, and the
  ``float-divide-by-zero`` check.
- With GCC: the ``bounds-strict``, ``float-cast-overflow``,
  ``pointer-compare``, and ``pointer-subtract`` checks.
- With Clang: the ``implicit-conversion``, ``local-bounds``,
  and ``nullability`` checks.

Clang doesn't get ``pointer-compare`` and ``pointer-subtract``,
since in optimized code they report comparisons and subtractions
that the optimizer added.

``ERNIC_USE_THREAD_SANITIZER`` builds them with
ThreadSanitizer. ThreadSanitizer can't be combined with
AddressSanitizer, so only one of these two options can be on.
Use a separate build directory for each.

Both options need GCC or Clang. Configuring checks that the
compiler can build a program with the sanitizers, and fails if,
for example, the sanitizer runtimes aren't installed:

.. code-block:: bash

   cmake -B build-asan -G Ninja -DCMAKE_C_COMPILER=clang \
     -DERNIC_USE_SANITIZERS=ON
   cmake --build build-asan
   ctest --test-dir build-asan

The sanitizers instrument everything rocm-ernic compiles.
Libraries that come from
the system, such as libvfio-user, GLib, and libibverbs, are not
instrumented.

Every sanitizer report is fatal, so a test with a report fails.
Each program sets some of the sanitizers' run-time options, such
as the one that turns on GCC's ``pointer-compare`` and
``pointer-subtract`` checks, and the one that makes
ThreadSanitizer stop at its first report (see
``cmake/sanitizer-default-options.c``). The ``ASAN_OPTIONS``,
``UBSAN_OPTIONS``, and ``TSAN_OPTIONS`` environment variables
override them.

``ERNIC_SANITIZER_SHARED_RUNTIME`` links programs with the
shared library version of the sanitizer runtime. Libraries from
another build that also uses the shared runtime, such as a
sanitizer build of libvfio-user, can then be used in the same
program. If the runtime isn't in a directory that the dynamic
linker searches, as with Clang's, the programs get an RPATH to
it. When the option is ``OFF``, programs link the static
runtime. It defaults to what the compiler does on its own:
``ON`` with GCC and ``OFF`` with Clang. Ubuntu's Clang 18
ignores the programs' default run-time options when they use
its shared runtime.

If ThreadSanitizer stops a program at startup with an
"unexpected memory mapping" error, the kernel's address space
layout randomization uses more bits than ThreadSanitizer
supports. Run the program under ``setarch "$(uname -m)" -R``,
as CI does, or reduce the randomization with
``sudo sysctl vm.mmap_rnd_bits=28``.

.. _code-coverage:

Code Coverage
-------------

``ERNIC_USE_CODE_COVERAGE`` instruments the server and the
test programs with LLVM source-based coverage. It needs Clang,
and the ``llvm-profdata`` and ``llvm-cov`` of the same LLVM
version (on Ubuntu, the ``llvm-<version>`` package). Each
instrumented program writes its profile to ``coverage/profraw``
in the build directory however it is started, so the servers
that the CTest scripts launch are counted too. The
``ernic-coverage`` target merges the profiles and reports the
coverage of ``src/``:

.. code-block:: bash

   cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang \
     -DCMAKE_BUILD_TYPE=Debug -DERNIC_USE_CODE_COVERAGE=ON
   cmake --build build
   ctest --test-dir build
   cmake --build build --target ernic-coverage

The report goes in ``coverage/report.txt`` (a summary for each
file) and ``coverage/lines.txt`` (the count for each line).
The profiles accumulate across runs; delete
``coverage/profraw`` to start over.

Build Debug, as above: optimized code's counts don't match its
source lines as closely. CI's Build and Test workflow runs one
coverage leg (Clang, Debug, no project-wide sanitizer). It puts
the report on the run's summary page and fails if line coverage
drops below the floor set in the workflow. On ``develop`` it also publishes the
figure to the ``badges`` branch, which the README's Coverage
badge reads.

.. _iwyu:

include-what-you-use
--------------------

``ERNIC_USE_IWYU`` runs `include-what-you-use
<https://include-what-you-use.org/>`_ (IWYU) on the server's
and the tests' sources as they compile, which prints the
``#include`` lines it suggests adding or removing. The
suggestions don't fail the build, and some are wrong.

IWYU parses with its own clang, which rejects compiler options
that it doesn't know, so build with the clang that IWYU is
built on. ``include-what-you-use --version`` prints its
version:

.. code-block:: bash

   cmake -B build-iwyu -G Ninja -DCMAKE_C_COMPILER=clang-19 \
     -DCMAKE_BUILD_TYPE=Debug -DERNIC_USE_IWYU=ON
   cmake --build build-iwyu

The mapping files ``cmake/iwyu-*.imp`` correct IWYU where it
names the wrong header. CI's IWYU workflow lists the files with
suggestions on the run's summary page, and fails only if IWYU
itself fails. ``AGENTS.md`` describes how to run that check
locally and which suggestions to ignore.

.. _clang-tidy:

clang-tidy
----------

``ERNIC_USE_CLANG_TIDY`` runs `clang-tidy
<https://clang.llvm.org/extra/clang-tidy/>`__ on the server's
and the tests' sources as they compile, with the checks in
``.clang-tidy``. Every finding is an error, so it fails the
build. It needs clang-tidy 19 or later, and Clang as the C
compiler, since clang-tidy rejects the options that the build
uses with GCC. Use the clang of the same version as clang-tidy:

.. code-block:: bash

   cmake -B build-tidy -G Ninja -DCMAKE_C_COMPILER=clang-23 \
     -DERNIC_USE_CLANG_TIDY=ON \
     -DERNIC_CLANG_TIDY_EXE=/usr/bin/clang-tidy-23
   cmake --build build-tidy -- -k 0

``-k 0`` keeps building after a file fails, so one build
reports every finding. CI's clang-tidy workflow builds Debug
and Release with clang-tidy 23 and lists the findings by check
on the run's summary page.

Guest ionic Modules
-------------------

The guest-side driver is the upstream
Linux ionic driver with the patches in
``ansible/files/patches/`` applied.
Configure with ``-DERNIC_BUILD_KMOD=ON`` to get the DKMS
targets, and run them in the guest:

.. code-block:: bash

   cmake -B build -G Ninja -DERNIC_BUILD_KMOD=ON
   cmake --build build --target fetch-ionic-sources
   cmake --build build --target build-ionic-dkms
   sudo cmake --build build --target install-ionic-dkms

:doc:`ionic` describes the patches, the pinned upstream ref,
and how to move to a newer baseline.

Building Documentation
----------------------

Documentation requires Doxygen and Python 3. A Python virtual
environment is created automatically in the build tree.

.. code-block:: bash

   cmake -B build -G Ninja -DERNIC_BUILD_DOCS=ON
   cmake --build build --target sphinx-html

The generated HTML is written to ``build/docs/html/``.

To build documentation without needing the project's library
dependencies (libvfio-user, glib, libibverbs):

.. code-block:: bash

   cmake -B build -DERNIC_DOCS_ONLY=ON \
     -DERNIC_BUILD_DOCS=ON
   cmake --build build --target sphinx-html
