"""Test support for driving a rocm-ernic server from Python.

The modules here stand in for a guest VM. They speak the vfio-user
protocol to a real ``rocm-ernic`` process over its Unix socket, so every
test built on them runs on a hosted CI runner with no VM, no RDMA
hardware, and no root:

  * :mod:`ernic.vfu` -- the vfio-user client and the memory a guest would
    have shared with the device.
  * :mod:`ernic.server` -- starting and stopping server processes, and
    holding them to a shutdown deadline.
  * :mod:`ernic.ionic` -- the ionic driver, as the guest kernel runs it:
    device bring-up, the admin queues, queue pairs, memory regions, work
    requests, and completions.
  * :mod:`ernic.mesh` -- the TCP mesh wire protocol, for standing in for
    a peer node or for a manager.

Copyright (C) Advanced Micro Devices, Inc.

SPDX-License-Identifier: GPL-2.0-or-later
"""
