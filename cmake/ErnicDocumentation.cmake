# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# ErnicDocumentation.cmake
# Sphinx + Breathe + Doxygen documentation pipeline
# (following ROCm best practices)
#
# A Python venv is created automatically in the build tree
# and populated from requirements.txt.

option(ERNIC_BUILD_DOCS
  "Build documentation with Sphinx + Breathe + Doxygen"
  OFF)

if(ERNIC_BUILD_DOCS)
    find_package(Doxygen REQUIRED)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    # ── Python venv with Sphinx + Breathe ──────────────
    set(ERNIC_DOCS_VENV "${PROJECT_BINARY_DIR}/docs-venv")
    set(ERNIC_DOCS_VENV_STAMP
      "${ERNIC_DOCS_VENV}/stamp")
    set(ERNIC_DOCS_REQUIREMENTS
      "${PROJECT_SOURCE_DIR}/requirements.txt")

    if(WIN32)
        set(ERNIC_VENV_BIN
          "${ERNIC_DOCS_VENV}/Scripts")
    else()
        set(ERNIC_VENV_BIN
          "${ERNIC_DOCS_VENV}/bin")
    endif()

    set(SPHINX_BUILD "${ERNIC_VENV_BIN}/sphinx-build")

    add_custom_command(
      OUTPUT ${ERNIC_DOCS_VENV_STAMP}
      COMMAND ${Python3_EXECUTABLE} -m venv
        ${ERNIC_DOCS_VENV}
      COMMAND ${ERNIC_VENV_BIN}/pip install
        --quiet --upgrade pip
      COMMAND ${ERNIC_VENV_BIN}/pip install
        --quiet -r ${ERNIC_DOCS_REQUIREMENTS}
      COMMAND ${CMAKE_COMMAND} -E touch
        ${ERNIC_DOCS_VENV_STAMP}
      DEPENDS ${ERNIC_DOCS_REQUIREMENTS}
      COMMENT "Creating docs venv and installing deps"
      VERBATIM
    )

    add_custom_target(docs-venv
      DEPENDS ${ERNIC_DOCS_VENV_STAMP}
      COMMENT "Ensure docs Python venv is up to date"
    )

    # ── Paths ──────────────────────────────────────────
    set(ERNIC_DOC_PATH "${PROJECT_BINARY_DIR}/docs")
    set(BREATHE_DOC_XML_DIR
      "${ERNIC_DOC_PATH}/xml")

    set(ERNIC_DOXYFILE_INPUT
      "${PROJECT_SOURCE_DIR}/src/rocm_ernic_internal.h"
      "${PROJECT_SOURCE_DIR}/src/rocm_ernic_compat.h"
      "${PROJECT_SOURCE_DIR}/src/ionic_eth_emu.h"
      "${PROJECT_SOURCE_DIR}/src/ionic_rdma_devcmd.h"
      "${PROJECT_SOURCE_DIR}/src/ionic_adminq.h"
      "${PROJECT_SOURCE_DIR}/src/ionic_datapath.h"
    )
    # Doxygen's INPUT is space-separated, not a CMake list
    list(JOIN ERNIC_DOXYFILE_INPUT " " ERNIC_DOXYFILE_INPUT)
    # Configure Doxyfile (substitutes @VARIABLES@)
    configure_file(
      ${PROJECT_SOURCE_DIR}/docs/Doxyfile.in
      ${PROJECT_BINARY_DIR}/Doxyfile
      @ONLY
    )

    # Configure conf.py (substitutes Breathe XML path)
    configure_file(
      ${PROJECT_SOURCE_DIR}/docs/conf.py
      ${PROJECT_BINARY_DIR}/docs-sphinx/conf.py
      @ONLY
    )

    # ── Doxygen target: source headers -> XML ──────────
    add_custom_target(doxygen
      COMMAND ${DOXYGEN_EXECUTABLE}
        ${PROJECT_BINARY_DIR}/Doxyfile
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
      COMMENT "Generating Doxygen XML"
      VERBATIM
    )

    # ── Sphinx target: RST + Doxygen XML -> HTML ───────
    add_custom_target(sphinx-html
      COMMAND ${SPHINX_BUILD}
        -b html
        -c ${PROJECT_BINARY_DIR}/docs-sphinx
        ${PROJECT_SOURCE_DIR}/docs
        ${ERNIC_DOC_PATH}/html
      DEPENDS doxygen docs-venv
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
      COMMENT "Building Sphinx HTML documentation"
      VERBATIM
    )
endif()
