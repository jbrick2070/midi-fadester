# visual-sidechain
# Copyright (C) 2026 Jeffrey A. Brick
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Fails when a locale key used in src/ through obs_module_text("...") or T_("...") is missing from the locale file
# (REQUIREMENTS R-TST-4). Run with:
#   cmake -DSOURCE_DIR=<repo> [-DLOCALE_FILE=<ini>] [-DEXTRA_SOURCE=<file>] -P check-locale-keys.cmake
# EXTRA_SOURCE adds one more file to scan; the negative ctest uses it to prove that a missing key fails.

# script mode sets no policies by itself, and if(... IN_LIST ...) needs CMP0057
cmake_minimum_required(VERSION 3.28)

if(NOT SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR is required")
endif()
if(NOT LOCALE_FILE)
  set(LOCALE_FILE "${SOURCE_DIR}/data/locale/en-US.ini")
endif()

file(GLOB_RECURSE _sources "${SOURCE_DIR}/src/*.c" "${SOURCE_DIR}/src/*.h")
if(EXTRA_SOURCE)
  list(APPEND _sources "${EXTRA_SOURCE}")
endif()

file(STRINGS "${LOCALE_FILE}" _locale_lines)
set(_keys "")
foreach(_line IN LISTS _locale_lines)
  if(_line MATCHES "^([^=;#]+)=")
    string(STRIP "${CMAKE_MATCH_1}" _key)
    list(APPEND _keys "${_key}")
  endif()
endforeach()

set(_missing "")
foreach(_source IN LISTS _sources)
  file(READ "${_source}" _text)
  string(REGEX MATCHALL "(obs_module_text|T_)\\([ \t]*\"[^\"]+\"[ \t]*\\)" _uses "${_text}")
  foreach(_use IN LISTS _uses)
    string(REGEX REPLACE "^(obs_module_text|T_)\\([ \t]*\"([^\"]+)\"[ \t]*\\)$" "\\2" _key "${_use}")
    if(NOT _key IN_LIST _keys)
      list(APPEND _missing "${_key} (${_source})")
    endif()
  endforeach()
endforeach()

if(_missing)
  list(REMOVE_DUPLICATES _missing)
  list(JOIN _missing "\n  " _report)
  message(FATAL_ERROR "locale keys missing from ${LOCALE_FILE}:\n  ${_report}")
endif()
list(LENGTH _sources _count)
message(STATUS "locale keys: every key used in ${_count} source files is in ${LOCALE_FILE}")
