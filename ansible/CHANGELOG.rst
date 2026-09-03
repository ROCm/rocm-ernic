======================================
sbates130272.rocm\_ernic Release Notes
======================================

.. contents:: Topics

v0.1.0
======

Release Summary
---------------

Initial release. The rocm-ernic VM and host provisioning
automation, previously only usable from inside the rocm-ernic
repository, is now packaged as a collection so a VM image can
be built for rocm-ernic from any project.

Major Changes
-------------

- Added ``ernic_guest_setup``, which installs the DKMS kernel driver, builds rdma-core with the ``rocm_ernic`` provider, applies the udev naming rules, addresses the emulated NIC and builds rocm-xio.
- Added ``ernic_host_setup``, which builds, installs and starts the rocm-ernic service, binds GPUs to ``vfio-pci`` and stages rocm-xio for the guests.
- Added ``ernic_image_prep``, which bakes RDMA userspace, ROCm, the build toolchain, ``modules-load.d`` entries and the ``pci.ids`` entry into a golden VM image.
- Added ``ernic_source``, which resolves the rocm-ernic checkout and clones it on the controller when no path is supplied.
