# Midi Fadester
# Copyright (C) 2026 Jeffrey A. Brick
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Fails when a locale key used in src/ is missing from en-US.ini, when the English text given in the code differs from
# en-US.ini's text, or when a key is built at run time where this check cannot see it (R-I18N-5). It reads
#   obs_module_text("Key")
#   mfd_text("Key", "English") and mfdText("Key", "English")
#   mfd_text_or_english(flag, "Key", "English")
# with the English split over several literals and lines as clang-format leaves it. Run with:
#   cmake -DSOURCE_DIR=<repo> [-DLOCALE_FILE=<ini>] [-DEXTRA_SOURCE=<file>] -P check-locale-keys.cmake
# EXTRA_SOURCE adds one more file to scan; the negative ctests use it to prove that each fault fails.

cmake_minimum_required(VERSION 3.28)

if(NOT SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR is required")
endif()
if(NOT LOCALE_FILE)
  set(LOCALE_FILE "${SOURCE_DIR}/data/locale/en-US.ini")
endif()

# Calls whose key is a variable on purpose: the hook itself. "<file name>|<call text>"; anything else fails.
set(_runtime_key_families "mfd-text.c|mfd_text(key," "mfd-text-qt.cpp|mfd_text(key,")

file(
  GLOB_RECURSE _sources
  "${SOURCE_DIR}/src/*.c"
  "${SOURCE_DIR}/src/*.h"
  "${SOURCE_DIR}/src/*.cpp"
  "${SOURCE_DIR}/src/*.hpp"
)
if(EXTRA_SOURCE)
  list(APPEND _sources "${EXTRA_SOURCE}")
endif()

# en-US.ini as OBS reads it: "#" starts a comment, key="text", \" \n \t \\ inside the text. Each text is kept as hex.
file(READ "${LOCALE_FILE}" _ini)
string(REPLACE "\r" "" _ini "${_ini}")
string(REPLACE ";" "\\x3b" _ini "${_ini}")
string(REPLACE "[" "\\x5b" _ini "${_ini}")
string(REPLACE "]" "\\x5d" _ini "${_ini}")
string(REPLACE "\n" ";" _ini_lines "${_ini}")

# hex of a text written with escapes (\xHH, \n, \t, \", \\, \'), the rest as UTF-8
function(escaped_to_hex text out)
  set(_hex "")
  string(REGEX MATCHALL "\\\\x[0-9A-Fa-f][0-9A-Fa-f]|\\\\.|[^\\\\]+" _parts "${text}")
  foreach(_part IN LISTS _parts)
    if(_part MATCHES "^\\\\x([0-9A-Fa-f][0-9A-Fa-f])$")
      string(TOLOWER "${CMAKE_MATCH_1}" _byte)
      string(APPEND _hex "${_byte}")
    elseif(_part STREQUAL "\\n")
      string(APPEND _hex "0a")
    elseif(_part STREQUAL "\\t")
      string(APPEND _hex "09")
    elseif(_part MATCHES "^\\\\(.)$")
      string(HEX "${CMAKE_MATCH_1}" _byte)
      string(APPEND _hex "${_byte}")
    else()
      string(HEX "${_part}" _bytes)
      string(APPEND _hex "${_bytes}")
    endif()
  endforeach()
  set(${out} "${_hex}" PARENT_SCOPE)
endfunction()

set(_keys "")
foreach(_line IN LISTS _ini_lines)
  if(_line MATCHES "^[ \t]*$" OR _line MATCHES "^[ \t]*#")
    continue()
  endif()
  if(_line MATCHES "^[ \t]*([^= \t]+)[ \t]*=[ \t]*\"(.*)\"[ \t]*$")
    set(_key "${CMAKE_MATCH_1}")
    escaped_to_hex("${CMAKE_MATCH_2}" _value_hex)
    list(APPEND _keys "${_key}")
    set(_text_${_key} "${_value_hex}")
  elseif(_line MATCHES "^[ \t]*([^= \t]+)[ \t]*=")
    string(STRIP "${CMAKE_MATCH_1}" _key)
    list(APPEND _keys "${_key}")
  endif()
endforeach()

set(_ws "[ \t\r\n]*")
set(_lit "\"([^\"\\\\]|\\\\.)*\"")
set(_hooked_call "(mfd_text|mfdText)\\(${_ws}\"[^\"]+\"${_ws},${_ws}(${_lit}${_ws})+\\)")
set(
  _flagged_call
  "mfd_text_or_english\\(${_ws}[A-Za-z_][A-Za-z0-9_.>-]*${_ws},${_ws}\"[^\"]+\"${_ws},${_ws}(${_lit}${_ws})+\\)"
)

set(_missing "")
set(_changed "")
set(_runtime "")
set(_unread "")
set(_name_start "[^A-Za-z0-9_]")
foreach(_source IN LISTS _sources)
  get_filename_component(_file_name "${_source}" NAME)
  file(READ "${_source}" _text)
  string(REPLACE ";" "\\x3b" _text "${_text}")
  string(REPLACE "[" "\\x5b" _text "${_text}")
  string(REPLACE "]" "\\x5d" _text "${_text}")
  # comments hold no calls: /* ... */ blocks, and // where a line or a statement ends (not "//" inside a string)
  string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" " " _text "${_text}")
  string(REGEX REPLACE "(^|\n)([ \t]*)//[^\n]*" "\\1\\2" _text "${_text}")
  string(REGEX REPLACE "([;{})])([ \t]*)//[^\n]*" "\\1\\2" _text "${_text}")
  set(_text "\n${_text}")

  # keys without an English text in the code
  string(REGEX MATCHALL "${_name_start}(obs_module_text|T_)\\(${_ws}\"[^\"]+\"${_ws}\\)" _uses "${_text}")
  foreach(_use IN LISTS _uses)
    string(REGEX REPLACE "^.(obs_module_text|T_)\\(${_ws}\"([^\"]+)\".*$" "\\2" _key "${_use}")
    if(NOT _key IN_LIST _keys)
      list(APPEND _missing "${_key} (${_source})")
    endif()
  endforeach()

  # keys with their English text: the text must be en-US.ini's
  string(REGEX MATCHALL "${_name_start}(${_hooked_call}|${_flagged_call})" _uses "${_text}")
  list(LENGTH _uses _read_count)
  foreach(_use IN LISTS _uses)
    string(SUBSTRING "${_use}" 1 -1 _call)
    string(REGEX REPLACE "^mfd_text_or_english\\(${_ws}[A-Za-z_][A-Za-z0-9_.>-]*${_ws}," "(" _call "${_call}")
    string(REGEX REPLACE "^(mfd_text|mfdText)" "" _call "${_call}")
    string(REGEX REPLACE "^\\(${_ws}\"([^\"]+)\".*$" "\\1" _key "${_call}")
    string(REGEX REPLACE "^\\(${_ws}\"[^\"]+\"${_ws},${_ws}" "" _english_part "${_call}")
    string(REGEX MATCHALL "${_lit}" _pieces "${_english_part}")
    set(_english "")
    foreach(_piece IN LISTS _pieces)
      string(REGEX REPLACE "^\"(.*)\"$" "\\1" _piece "${_piece}")
      string(APPEND _english "${_piece}")
    endforeach()
    if(NOT _key IN_LIST _keys)
      list(APPEND _missing "${_key} (${_source})")
      continue()
    endif()
    escaped_to_hex("${_english}" _english_hex)
    if(NOT DEFINED _text_${_key} OR NOT _english_hex STREQUAL _text_${_key})
      list(APPEND _changed "${_key} (${_source})")
    endif()
  endforeach()

  # keys built at run time: a hook call whose key is not a literal
  string(REGEX MATCHALL "${_name_start}(mfd_text|mfdText)\\(${_ws}[^\" \t\r\n][^,)]*[,)]" _calls "${_text}")
  string(
    REGEX MATCHALL
    "${_name_start}mfd_text_or_english\\(${_ws}[A-Za-z_][A-Za-z0-9_.>-]*${_ws},${_ws}[^\" \t\r\n][^,)]*[,)]"
    _flagged
    "${_text}"
  )
  list(APPEND _calls ${_flagged})
  foreach(_call IN LISTS _calls)
    string(SUBSTRING "${_call}" 1 -1 _call)
    string(REGEX REPLACE "[ \t\r\n]+" " " _call "${_call}")
    # declarations and definitions name a type first
    if(_call MATCHES "^[A-Za-z_]+\\( ?(const |bool |QString |QString&|char |void)")
      continue()
    endif()
    math(EXPR _read_count "${_read_count} + 1")
    set(_allowed FALSE)
    foreach(_family IN LISTS _runtime_key_families)
      string(REPLACE "|" ";" _family_parts "${_family}")
      list(GET _family_parts 0 _family_file)
      list(GET _family_parts 1 _family_call)
      string(FIND "${_call}" "${_family_call}" _at)
      if(_file_name STREQUAL _family_file AND _at EQUAL 0)
        set(_allowed TRUE)
      endif()
    endforeach()
    if(NOT _allowed)
      list(APPEND _runtime "${_call} (${_source})")
    endif()
  endforeach()

  # every hook call must be one of the forms above, else its key or English goes unchecked
  string(
    REGEX MATCHALL
    "${_name_start}(mfd_text|mfdText|mfd_text_or_english)[ \t\r\n]*\\([ \t\r\n]*[^ \t\r\n]*"
    _all
    "${_text}"
  )
  set(_call_count 0)
  foreach(_call IN LISTS _all)
    string(REGEX REPLACE "[ \t\r\n]+" " " _call "${_call}")
    if(NOT _call MATCHES "\\( ?(const|bool|QString|QString&|char|void)$")
      math(EXPR _call_count "${_call_count} + 1")
    endif()
  endforeach()
  if(_call_count GREATER _read_count)
    math(EXPR _skipped "${_call_count} - ${_read_count}")
    list(APPEND _unread "${_skipped} call(s) in ${_source}")
  endif()
endforeach()

set(_report "")
if(_missing)
  list(REMOVE_DUPLICATES _missing)
  list(JOIN _missing "\n  " _lines)
  string(APPEND _report "locale keys missing from ${LOCALE_FILE}:\n  ${_lines}\n")
endif()
if(_changed)
  list(REMOVE_DUPLICATES _changed)
  list(JOIN _changed "\n  " _lines)
  string(APPEND _report "English in the code differs from ${LOCALE_FILE}:\n  ${_lines}\n")
endif()
if(_runtime)
  list(REMOVE_DUPLICATES _runtime)
  list(JOIN _runtime "\n  " _lines)
  string(APPEND _report "locale keys built at run time (list the family in check-locale-keys.cmake):\n  ${_lines}\n")
endif()
if(_unread)
  list(JOIN _unread "\n  " _lines)
  string(
    APPEND
    _report
    "hook calls the key check cannot read (write the key and the English as plain literals):\n  ${_lines}\n"
  )
endif()
if(_report)
  message(FATAL_ERROR "${_report}")
endif()
list(LENGTH _sources _count)
list(LENGTH _keys _key_count)
message(STATUS "locale keys: every key used in ${_count} source files is among the ${_key_count} in ${LOCALE_FILE}")
