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
html_static_path = []
