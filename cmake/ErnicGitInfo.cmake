# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Extract git SHA information at configure time for embedding
# in the binary. Falls back to safe defaults when git is not
# available (e.g. tarball builds).

find_program(GIT_EXECUTABLE git)

set(GIT_SHA_FULL
  "0000000000000000000000000000000000000000"
  CACHE STRING "Full git SHA of the current commit")
set(GIT_SHA_SHORT
  "unknown"
  CACHE STRING "Short git SHA of the current commit")

if(GIT_EXECUTABLE)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE _git_sha_full
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _git_result
    )
    if(_git_result EQUAL 0)
        set(GIT_SHA_FULL "${_git_sha_full}"
          CACHE STRING "Full git SHA of the current commit"
          FORCE)

        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --short=8 HEAD
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE _git_sha_short
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _git_short_result
        )
        if(_git_short_result EQUAL 0)
            set(GIT_SHA_SHORT "${_git_sha_short}"
              CACHE STRING
              "Short git SHA of the current commit"
              FORCE)
        endif()
    endif()
endif()

message(STATUS "Git SHA: ${GIT_SHA_SHORT} (${GIT_SHA_FULL})")
