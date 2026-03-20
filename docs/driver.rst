Kernel Driver
=============

The ``rocm_ernic`` Linux kernel module is the guest-side
companion to the userspace server. It registers an InfiniBand
device so that standard RDMA applications (libibverbs, librdmacm)
work inside the virtual machine.

Building the Driver
-------------------

.. code-block:: bash

   cd driver/

   # Build against the running kernel (KDIR is optional)
   make -C $KDIR M=$(pwd) modules

   # Install the module (optional)
   sudo make -C $KDIR M=$(pwd) modules_install
   sudo depmod -a

Loading the Driver
------------------

.. code-block:: bash

   sudo modprobe rocm_ernic

   # Verify with dmesg
   dmesg | grep rocm_ernic

Expected output when the device is detected:

::

   [  xxx] rocm_ernic 0000:00:04.0: device version 17, \
   driver version 20
   [  xxx] rocm_ernic 0000:00:04.0: DSR initialized \
   after N polls
   [  xxx] rocm_ernic 0000:00:04.0: running in standalone \
   mode (no netdev)
   [  xxx] rocm_ernic 0000:00:04.0: registered ibdev \
   rocm_ernicX

Verifying the Device
--------------------

After loading the driver, confirm that the PCI device and
the InfiniBand device are visible:

.. code-block:: bash

   lspci | grep 1022:1488
   ibv_devices

End-to-End Workflow
-------------------

1. Start the server on the host:

   .. code-block:: bash

      ./build/rocm-ernic \
        --socket /tmp/vfio-user-rocm-ernic.sock \
        --backend loopback --verbose

2. Launch QEMU with the vfio-user device attached
   (see :doc:`usage` for the full command line).

3. Inside the guest, load the driver:

   .. code-block:: bash

      sudo modprobe rocm_ernic

4. Run an RDMA application or test:

   .. code-block:: bash

      ibv_devices
      ibv_devinfo -d rocm_ernic0
