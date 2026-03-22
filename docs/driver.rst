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

Sysfs Loopback Mode
-------------------

The driver exposes a ``loopback`` sysfs attribute on both the
Ethernet PCI device and the InfiniBand device. When enabled,
the driver operates without a real network path: TX packets
are reflected back as RX at the Ethernet layer, and the RDMA
module skips GID binding commands. This is useful for testing
RDMA applications in the guest without a fully configured
userspace server backend.

**Ethernet-level loopback** (TX packets reflected as RX):

.. code-block:: bash

   # Enable
   echo 1 > /sys/class/net/rocm_ernic0/device/loopback

   # Disable
   echo 0 > /sys/class/net/rocm_ernic0/device/loopback

   # Check current state
   cat /sys/class/net/rocm_ernic0/device/loopback

**RDMA-level loopback** (skips GID binding, syncs with
Ethernet loopback):

.. code-block:: bash

   # Enable (also enables Ethernet loopback)
   echo 1 > /sys/class/infiniband/rocm_ernic0/loopback

   # Disable
   echo 0 > /sys/class/infiniband/rocm_ernic0/loopback

   # Check current state
   cat /sys/class/infiniband/rocm_ernic0/loopback

The RDMA-level attribute is the primary control; writing to
it automatically propagates to the Ethernet module. Writing
directly to the Ethernet attribute only affects the Ethernet
layer.

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
