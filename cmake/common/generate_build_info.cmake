# Generate build-time git commit and timestamp header
# Copyright (C) 2026 Jeffrey A. Brick
# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT DEFINED SOURCE_DIR OR NOT DEFINED OUTPUT_HEADER)
  message(FATAL_ERROR "SOURCE_DIR and OUTPUT_HEADER must be provided")
endif()

set(_git_commit "unknown")
set(_built_at "")

find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" rev-parse --short=7 HEAD
    RESULT_VARIABLE _rev_rc
    OUTPUT_VARIABLE _rev_out
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(_rev_rc EQUAL 0 AND NOT _rev_out STREQUAL "")
    set(_git_commit "${_rev_out}")

    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" status --porcelain --untracked-files=no
      RESULT_VARIABLE _st_rc
      OUTPUT_VARIABLE _st_out
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(_st_rc EQUAL 0 AND NOT _st_out STREQUAL "")
      set(_git_commit "${_git_commit}-dirty")
    else()
      execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" log -1 "--format=%cd" "--date=format:%Y-%m-%d %H:%M" HEAD
        RESULT_VARIABLE _dt_rc
        OUTPUT_VARIABLE _dt_out
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
      )
      if(_dt_rc EQUAL 0 AND NOT _dt_out STREQUAL "")
        set(_built_at "${_dt_out}")
      endif()
    endif()
  endif()
endif()

if(_built_at STREQUAL "")
  string(TIMESTAMP _built_at "%Y-%m-%d %H:%M")
endif()

string(
  CONCAT _header_content
  "/* Auto-generated at build time by generate_build_info.cmake */\n"
  "#pragma once\n"
  "#define MFD_BUILD_GIT_COMMIT \"${_git_commit}\"\n"
  "#define MFD_BUILD_TIMESTAMP \"${_built_at}\"\n"
)

cmake_path(GET OUTPUT_HEADER PARENT_PATH _out_dir)
file(MAKE_DIRECTORY "${_out_dir}")

set(_should_write TRUE)
if(EXISTS "${OUTPUT_HEADER}")
  file(READ "${OUTPUT_HEADER}" _existing_content)
  if(_existing_content STREQUAL _header_content)
    set(_should_write FALSE)
  endif()
endif()

if(_should_write)
  file(WRITE "${OUTPUT_HEADER}" "${_header_content}")
endif()
