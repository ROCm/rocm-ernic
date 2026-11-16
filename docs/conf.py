# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Sphinx reads its settings from lowercase module-level names
# pylint: disable=invalid-name

"""Sphinx configuration for rocm-ernic documentation."""

project = "rocm-ernic"
author = "Advanced Micro Devices, Inc."
# Sphinx's setting is called copyright
# pylint: disable-next=redefined-builtin
copyright = "2025-2026 Advanced Micro Devices, Inc. All rights reserved."

version = "0.2.0"
release = version

# -- Repository --------------------------------------------------

# Where the sources are on GitHub. Every link the pages make into the
# repository is built from these, so a move changes only them.
repository_url = "https://github.com/ROCm/rocm-ernic"
repository_branch = "develop"
# The project's directory in the repository, with a trailing slash,
# or "" when the project is the whole repository.
project_dir = ""

# Links into the repository, as roles:
#
#   :project-file:`LICENSE.md`               a file in the project
#   :project-file:`license <LICENSE.md>`     ... with its own text
#   :repo:`nightly <releases/tag/nightly>`   a page of the repository
#
# A substitution can't be used inside a link's target, which is why
# these are roles rather than rst_epilog substitutions.
extlinks = {
    "project-file": (
        f"{repository_url}/blob/{repository_branch}/{project_dir}%s",
        "%s",
    ),
    "repo": (f"{repository_url}/%s", "%s"),
}

# Substitutions for repository names in text and in literal blocks.
# A code-block doesn't expand them; use parsed-literal, which does.
#
#   |repository|          ``ROCm/rocm-ernic``
#   |collection-source|   the Ansible collection's source, for
#                         ansible-galaxy collection install
repository_name = repository_url.removeprefix("https://github.com/")
rst_epilog = f"""
.. |repository| replace:: ``{repository_name}``
.. |collection-source| replace:: git+{repository_url}.git#/{project_dir}ansible/
"""

# -- Extensions --------------------------------------------------

extensions = [
    "breathe",
    "sphinx.ext.extlinks",
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

# generated/ holds the counter lists tools/gen-counter-docs.py builds
# from the C sources.  They are included into monitoring.rst, not pages
# of their own, and Sphinx warns about any .rst it collects that no
# toctree names.  An excluded file can still be ``include``d.
exclude_patterns = ["_build", "Thumbs.db", ".DS_Store", "generated"]
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
    "repository_url": repository_url,
    "repository_branch": repository_branch,
    "path_to_docs": f"{project_dir}docs",
    "use_repository_button": True,
    "show_toc_level": 2,
}
html_title = f"rocm-ernic {version}"
# No user static directory. docs/perf-history used to be published
# here, on the understanding that it was how README.md's shields
# reached a stable URL. It was not: the shields point at
# perf/badge-*.json on gh-pages, written directly by
# .github/actions/ernic-publish-perf, and the _static/ copies were
# built from whatever was committed to develop -- the "no data"
# placeholders, permanently, with no badge-s3.json at all once the S3
# lane was added. Nothing read them.
#
# The charts do not need this either: publish-perf.py includes them
# with `.. raw:: html :file: perf-history/chart-*.html`, resolved
# against the source tree and inlined into the page.
#
# Set explicitly rather than left to Sphinx's ["_static"] default,
# which would warn on every build about a directory this project
# does not have.
html_static_path = []
