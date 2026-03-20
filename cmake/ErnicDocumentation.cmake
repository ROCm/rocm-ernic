# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: GPL-2.0-or-later

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
    set(ERNIC_DOCS_VENV "${CMAKE_BINARY_DIR}/docs-venv")
    set(ERNIC_DOCS_VENV_STAMP
      "${ERNIC_DOCS_VENV}/stamp")
    set(ERNIC_DOCS_REQUIREMENTS
      "${CMAKE_SOURCE_DIR}/requirements.txt")

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
    set(ERNIC_DOC_PATH "${CMAKE_BINARY_DIR}/docs")
    set(BREATHE_DOC_XML_DIR
      "${ERNIC_DOC_PATH}/xml")

    set(ERNIC_DOXYFILE_INPUT
      "${CMAKE_SOURCE_DIR}/src/rocm_ernic_internal.h \
       ${CMAKE_SOURCE_DIR}/src/rocm_ernic_compat.h \
       ${CMAKE_SOURCE_DIR}/src/rocm_ernic_eth.h \
       ${CMAKE_SOURCE_DIR}/driver/rocm_ernic_dev_api.h \
       ${CMAKE_SOURCE_DIR}/driver/rocm_ernic-abi.h \
       ${CMAKE_SOURCE_DIR}/driver/rocm_ernic_verbs.h \
       ${CMAKE_SOURCE_DIR}/driver/rocm_ernic_pci_ids.h \
       ${CMAKE_SOURCE_DIR}/driver/rocm_ernic.h \
       ${CMAKE_SOURCE_DIR}/driver/rocm_ernic_ring.h"
    )

    # Configure Doxyfile (substitutes @VARIABLES@)
    configure_file(
      ${CMAKE_SOURCE_DIR}/docs/Doxyfile.in
      ${CMAKE_BINARY_DIR}/Doxyfile
      @ONLY
    )

    # Configure conf.py (substitutes Breathe XML path)
    configure_file(
      ${CMAKE_SOURCE_DIR}/docs/conf.py
      ${CMAKE_BINARY_DIR}/docs-sphinx/conf.py
      @ONLY
    )

    # ── Doxygen target: source headers -> XML ──────────
    add_custom_target(doxygen
      COMMAND ${DOXYGEN_EXECUTABLE}
        ${CMAKE_BINARY_DIR}/Doxyfile
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "Generating Doxygen XML"
      VERBATIM
    )

    # ── Sphinx target: RST + Doxygen XML -> HTML ───────
    add_custom_target(sphinx-html
      COMMAND ${SPHINX_BUILD}
        -b html
        -c ${CMAKE_BINARY_DIR}/docs-sphinx
        ${CMAKE_SOURCE_DIR}/docs
        ${ERNIC_DOC_PATH}/html
      DEPENDS doxygen docs-venv
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "Building Sphinx HTML documentation"
      VERBATIM
    )
endif()
