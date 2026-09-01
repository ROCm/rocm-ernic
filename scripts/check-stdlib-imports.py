#!/usr/bin/env python3
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# check-stdlib-imports.py
#
# Verify that a Python script imports only from the
# standard library.  ernicctl ships as a single file that
# has to run on a bare host with no pip packages
# available, so a stray third-party import breaks it in
# exactly the environments it exists to manage.
#
# Usage:
#   scripts/check-stdlib-imports.py [FILE...]
#
# Defaults to service/ernicctl.  Exits non-zero and lists
# the offending module names if any are found.

import ast
import sys

DEFAULT_TARGETS = ["service/ernicctl"]


def offending_imports(path):
    """Return the sorted non-stdlib top-level modules."""
    with open(path) as fh:
        tree = ast.parse(fh.read(), filename=path)

    stdlib = set(sys.stdlib_module_names)
    bad = set()

    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                top = alias.name.split(".")[0]
                if top not in stdlib:
                    bad.add(top)
        elif isinstance(node, ast.ImportFrom):
            # A relative import (level > 0) has no module
            # name to resolve and is not a stdlib question.
            if node.module and not node.level:
                top = node.module.split(".")[0]
                if top not in stdlib:
                    bad.add(top)

    return sorted(bad)


def main(argv):
    targets = argv[1:] or DEFAULT_TARGETS
    failed = False

    for path in targets:
        try:
            bad = offending_imports(path)
        except (OSError, SyntaxError) as exc:
            print(f"{path}: could not parse: {exc}")
            failed = True
            continue

        if bad:
            print(f"{path}: non-stdlib imports: {bad}")
            failed = True
        else:
            print(f"{path}: all imports are stdlib.")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
