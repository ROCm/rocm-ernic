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

By default the binary installs to ``/usr/local/bin/rocm-ernic``.
Override the destination with ``-DCMAKE_INSTALL_PREFIX=<path>``.

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
   * - ``ERNIC_USE_SANITIZERS``
     - ``OFF``
     - Enable ASAN / LSAN / UBSAN
   * - ``ERNIC_USE_THREAD_SANITIZER``
     - ``OFF``
     - Enable TSAN (mutually exclusive with above)
   * - ``ERNIC_BUILD_DOCS``
     - ``OFF``
     - Build Sphinx + Breathe + Doxygen documentation
   * - ``ERNIC_DOCS_ONLY``
     - ``OFF``
     - Configure only documentation targets (no library
       dependencies required)
   * - ``CMAKE_INSTALL_PREFIX``
     - ``/usr/local``
     - Installation prefix

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
