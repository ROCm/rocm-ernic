# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Sphinx reads its settings from lowercase module-level names
# pylint: disable=invalid-name

"""Sphinx configuration for rocm-ernic documentation."""

project = "rocm-ernic"
author = "Advanced Micro Devices, Inc."
# pylint: disable=redefined-builtin
copyright = "2025-2026 Advanced Micro Devices, Inc. All rights reserved."
# pylint: enable=redefined-builtin

version = "0.2.0"
release = version

# -- Extensions --------------------------------------------------

extensions = [
    "breathe",
    # Emits .nojekyll into the build.  Pages serves this site from a
    # branch, so without it Jekyll runs and strips _static/ -- the
    # whole site loses its CSS.
    "sphinx.ext.githubpages",
]

# -- Breathe (Doxygen XML import) --------------------------------

breathe_projects = {
    "rocm-ernic": "@BREATHE_DOC_XML_DIR@",
}
breathe_default_project = "rocm-ernic"
breathe_default_members = ("members",)

# -- General -----------------------------------------------------

exclude_patterns = ["_build", "Thumbs.db", ".DS_Store"]
templates_path = []

# Kernel driver headers reuse type names across files and
# use anonymous unions that Breathe cannot fully parse.
suppress_warnings = [
    "cpp.duplicate_declaration",
    "c.duplicate_declaration",
]

# -- HTML output -------------------------------------------------

html_theme = "sphinx_book_theme"
html_theme_options = {
    "repository_url": ("https://github.com/ROCm/rocm-ernic"),
    "use_repository_button": True,
    "show_toc_level": 2,
}
html_title = f"rocm-ernic {version}"
# No user static directory. docs/perf-history used to be published
# here, on the understanding that it was how README.md's shields
# reached a stable URL. It was not: the shields point at
# perf/badge-*.json on gh-pages, written directly by
# .github/actions/publish-perf, and the _static/ copies were built
# from whatever was committed to develop -- the "no data" placeholders,
# permanently, with no badge-s3.json at all once the S3 lane was
# added. Nothing read them.
#
# The charts do not need this either: publish-perf.py includes them
# with `.. raw:: html :file: perf-history/chart-*.html`, resolved
# against the source tree and inlined into the page.
#
# Set explicitly rather than left to Sphinx's ["_static"] default,
# which would warn on every build about a directory this project
# does not have.
html_static_path = []
