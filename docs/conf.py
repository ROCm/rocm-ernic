# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Sphinx configuration for rocm-ernic documentation."""

project = "rocm-ernic"
author = "Advanced Micro Devices, Inc."
copyright = (
    "2025-2026 Advanced Micro Devices, Inc. "
    "All rights reserved."
)

version = "0.2.0"
release = version

# -- Extensions --------------------------------------------------

extensions = [
    "breathe",
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
    "repository_url": (
        "https://github.com/ROCm/rocm-ernic"
    ),
    "use_repository_button": True,
    "show_toc_level": 2,
}
html_title = f"rocm-ernic {version}"
# docs/perf-history holds the nightly perf charts and the
# shields.io endpoint badges (badge-rdma.json, badge-tcp.json)
# that README.md points at. Publishing it as html_static_path
# copies its contents into the built site's _static/, so once
# docs-deploy pushes the build to GitHub Pages the badges are
# reachable at a stable URL. sphinx-build is invoked with -c
# pointing at a separate configured-conf.py directory (see
# cmake/ErnicDocumentation.cmake), so this path must be
# absolute rather than relative to the docs/ source tree.
html_static_path = ["@CMAKE_SOURCE_DIR@/docs/perf-history"]
