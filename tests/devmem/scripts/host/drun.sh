#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# docker run wrapper for this host: dockerd here refuses a container without
# an explicit cgroup parent, and attached stdout is not delivered, so run
# detached-style, wait, then print the logs.   drun.sh [docker run args...]
set -euo pipefail
id=$(docker create --cgroup-parent=system.slice "$@")
docker start "$id" >/dev/null
rc=$(docker wait "$id")
docker logs "$id" 2>&1
docker rm "$id" >/dev/null
exit "$rc"
