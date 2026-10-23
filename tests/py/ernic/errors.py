"""Errors shared by the driver modules.

In its own module so that :mod:`ernic.eth` and :mod:`ernic.ionic` can both
raise it without importing each other.

Copyright (C) Advanced Micro Devices, Inc.

SPDX-License-Identifier: GPL-2.0-or-later
"""


class DeviceError(Exception):
    """The device refused a command or never answered one."""
