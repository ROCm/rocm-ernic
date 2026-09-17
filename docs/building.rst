Building and Installing
=======================

Dependencies
------------

Install the required packages on Ubuntu/Debian:

.. code-block:: bash

   sudo apt install cmake meson ninja-build pkg-config \
     libibverbs-dev librdmacm-dev libglib2.0-dev

Build and install ``libvfio-user`` if it is not already
available on your system:

.. code-block:: bash

   cd /path/to/libvfio-user
   meson setup build --prefix=/usr
   ninja -C build
   sudo ninja -C build install
   sudo ldconfig

Compilation
-----------

From the project root directory:

.. code-block:: bash

   cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
   cmake --build build

The executable is produced at ``build/rocm-ernic``.

Installation
------------

.. code-block:: bash

   sudo cmake --install build

By default the binary installs to ``/usr/local/bin/rocm-ernic``,
and that is the only file a default install places.
Override the destination with ``-DCMAKE_INSTALL_PREFIX=<path>``.

Configure with ``-DERNIC_INSTALL_SERVICE=ON`` to additionally
install the systemd units, ``ernicctl``, the launcher, the
Prometheus exporter, and the ionic driver patches;
:doc:`service` lists the full set of installed files.

Build Options
-------------

.. list-table::
   :header-rows: 1
   :widths: 30 10 60

   * - Option
     - Default
     - Description
   * - ``CMAKE_BUILD_TYPE``
     - ``Debug``
     - Build type (Debug, Release, RelWithDebInfo, etc.)
   * - ``ERNIC_WERROR``
     - ``OFF``
     - Treat compiler warnings as errors for project code.
       Off by default so a build with an unfamiliar compiler
       is never broken by a new warning; CI turns it on
       (see :ref:`werror-policy`)
   * - ``ERNIC_USE_SANITIZERS``
     - ``OFF``
     - Enable ASAN / LSAN / UBSAN
   * - ``ERNIC_USE_THREAD_SANITIZER``
     - ``OFF``
     - Enable TSAN (mutually exclusive with above)
   * - ``ERNIC_TEST_DISABLE_ASLR``
     - ``ON``
     - Run the TSAN tests under ``setarch -R``. Without it TSan
       aborts before ``main()`` with "unexpected memory mapping"
       on hosts with high mmap entropy. Turn off if you have set
       ``vm.mmap_rnd_bits=28`` instead. No effect unless
       ``ERNIC_USE_THREAD_SANITIZER`` is on
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
     - ``v7.2.4``
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
   * - ``CMAKE_INSTALL_PREFIX``
     - ``/usr/local``
     - Installation prefix

.. _werror-policy:

Warnings as Errors
------------------

``ERNIC_WERROR`` adds ``-Werror`` to the project's own
sources and to the test targets. The QEMU-ported sources
under ``src/from-qemu/`` are compiled with ``-w`` regardless,
so the flag only governs code this project maintains.

It defaults to ``OFF`` so that a packager or downstream
consumer building with a compiler version we have not tested
is never blocked by a newly-introduced warning. That
tolerance is not extended to our own CI: every lane that
compiles the project configures with ``-DERNIC_WERROR=ON``,
so a new warning fails the build before it can merge.

Developers should build with it on, matching CI:

.. code-block:: bash

   cmake -B build -G Ninja -DERNIC_WERROR=ON

The ``nix`` static-analysis, dynamic-analysis, and fuzz
builds are the one exception; they configure with it off
because gcc 15 and current clang emit warnings on the
QEMU-ported headers that these builds cannot suppress.
Those lanes are informational and do not gate merges.

Guest ionic Modules
-------------------

The guest-side driver is the upstream
Linux ionic driver with the patches in ``patches/`` applied.
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
