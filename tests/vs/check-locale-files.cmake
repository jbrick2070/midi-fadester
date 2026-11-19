# visual-sidechain
# Copyright (C) 2026 Jeffrey A. Brick
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Fails when a translation in the locale folder does not match en-US.ini (REQUIREMENTS R-I18N-4): a key missing, extra
# or repeated, a format text whose printf conversions differ from the English (R-I18N-2: the same conversions, in the
# same order and number; "%%" is no conversion; any other "%" is refused), a line OBS would read differently, a file
# named other than xx-YY.ini, a file that is not UTF-8 or starts with a BOM, or (with REQUIRED_LOCALES) a language that
# has no file. Format texts are the keys of every vs_text_format("Key", ...) call under SOURCE_DIR/src, plus
# FORMAT_KEYS; other texts may hold a plain "%" ("from 0% to 100%"). Run with:
#   cmake -DLOCALE_DIR=<folder with en-US.ini> [-DSOURCE_DIR=<repo>] [-DFORMAT_KEYS=A,B] [-DREQUIRED_LOCALES=de-DE,...]
#     -P check-locale-files.cmake
# The negative ctests use check-locale-files-bad.cmake to prove each failure is caught.

cmake_minimum_required(VERSION 3.28)

if(NOT LOCALE_DIR)
  message(FATAL_ERROR "LOCALE_DIR is required")
endif()

# characters CMake lists treat specially stand in for themselves while lines are split; nothing here looks at them
string(ASCII 1 _semicolon)
string(ASCII 2 _open)
string(ASCII 3 _close)

# key -> message, for one file, as OBS's text lookup reads it: "#" starts a comment, key = "text", and \" inside the
# text; the keys go to <prefix>_KEYS and each message to <prefix>_<key>
function(read_locale file prefix)
  get_filename_component(_name "${file}" NAME)
  file(READ "${file}" _text)
  string(REPLACE "\r" "" _text "${_text}")
  string(REPLACE ";" "${_semicolon}" _text "${_text}")
  string(REPLACE "[" "${_open}" _text "${_text}")
  string(REPLACE "]" "${_close}" _text "${_text}")
  string(REPLACE "\n" ";" _lines "${_text}")
  set(_keys "")
  foreach(_line IN LISTS _lines)
    if(_line MATCHES "^[ \t]*$" OR _line MATCHES "^[ \t]*#")
      continue()
    endif()
    if(NOT _line MATCHES "^[ \t]*([^= \t]+)[ \t]*=[ \t]*\"(.*)\"[ \t]*$")
      message(SEND_ERROR "${_name}: not key=\"text\": ${_line}")
      continue()
    endif()
    set(_key "${CMAKE_MATCH_1}")
    set(_value "${CMAKE_MATCH_2}")
    # OBS ends the text at the first quote that is not escaped, so a later one would be lost
    string(REPLACE "\\\\" "" _unescaped "${_value}")
    string(REPLACE "\\\"" "" _unescaped "${_unescaped}")
    if(_unescaped MATCHES "\"")
      message(SEND_ERROR "${_name}: ${_key} has a quote that is not escaped")
    endif()
    if(_key IN_LIST _keys)
      message(SEND_ERROR "${_name}: key ${_key} appears more than once")
    endif()
    list(APPEND _keys "${_key}")
    set(${prefix}_${_key} "${_value}" PARENT_SCOPE)
  endforeach()
  set(${prefix}_KEYS "${_keys}" PARENT_SCOPE)
endfunction()

# the printf conversions of a format text, in order and with repeats ("%s;%.0f"), with the same rules as
# src/core/text-format.c: only %s, %d, %u, %f, %g and %.Nf, %.Ng; "%%" is skipped whole (a marker stands in for it, so
# the characters around it never join into a conversion); any other "%" adds "BAD", which matches no English
string(ASCII 4 _percent_pair)
set(_conversion "%(\\.[0-9]+[fg]|[sdufg])")
function(conversions text out)
  string(REPLACE "%%" "${_percent_pair}" _text "${text}")
  string(REGEX MATCHALL "${_conversion}" _found "${_text}")
  string(REGEX REPLACE "${_conversion}" "" _rest "${_text}")
  if(_rest MATCHES "%")
    list(APPEND _found "BAD")
  endif()
  set(${out} "${_found}" PARENT_SCOPE)
endfunction()

# the format texts: every vs_text_format("Key", ...) in the sources, and FORMAT_KEYS
set(_format_keys "")
if(FORMAT_KEYS)
  string(REPLACE "," ";" _format_keys "${FORMAT_KEYS}")
endif()
if(SOURCE_DIR)
  file(GLOB_RECURSE _sources "${SOURCE_DIR}/src/*.c" "${SOURCE_DIR}/src/*.h")
  foreach(_source IN LISTS _sources)
    file(READ "${_source}" _text)
    string(REGEX MATCHALL "vs_text_format\\([ \t\r\n]*\"[^\"]+\"" _uses "${_text}")
    foreach(_use IN LISTS _uses)
      string(REGEX REPLACE "^vs_text_format\\([ \t\r\n]*\"([^\"]+)\"$" "\\1" _key "${_use}")
      list(APPEND _format_keys "${_key}")
    endforeach()
  endforeach()
endif()

# UTF-8 without BOM: each valid character (exact ranges, so no overlong forms, surrogates or code points past
# U+10FFFF), longest first, becomes a marker, so removing one can never join the bytes around it into another
function(check_encoding file)
  get_filename_component(_name "${file}" NAME)
  file(READ "${file}" _hex HEX)
  if(_hex MATCHES "^efbbbf")
    message(SEND_ERROR "${_name}: starts with a BOM")
  endif()
  set(_c "[89ab][0-9a-f]")
  string(REGEX REPLACE "(..)" "\\1 " _bytes "${_hex}")
  string(REGEX REPLACE "f0 (9[0-9a-f]|[ab][0-9a-f]) ${_c} ${_c} " "| " _bytes "${_bytes}")
  string(REGEX REPLACE "f[1-3] ${_c} ${_c} ${_c} " "| " _bytes "${_bytes}")
  string(REGEX REPLACE "f4 8[0-9a-f] ${_c} ${_c} " "| " _bytes "${_bytes}")
  string(REGEX REPLACE "e0 [ab][0-9a-f] ${_c} " "| " _bytes "${_bytes}")
  string(REGEX REPLACE "(e[1-9a-c]|e[ef]) ${_c} ${_c} " "| " _bytes "${_bytes}")
  string(REGEX REPLACE "ed [89][0-9a-f] ${_c} " "| " _bytes "${_bytes}")
  string(REGEX REPLACE "(c[2-9a-f]|d[0-9a-f]) ${_c} " "| " _bytes "${_bytes}")
  string(REGEX REPLACE "[0-7][0-9a-f] " "| " _bytes "${_bytes}")
  string(REPLACE "| " "" _bytes "${_bytes}")
  if(NOT _bytes STREQUAL "")
    message(SEND_ERROR "${_name}: not valid UTF-8 (bytes left: ${_bytes})")
  endif()
endfunction()

read_locale("${LOCALE_DIR}/en-US.ini" EN)
check_encoding("${LOCALE_DIR}/en-US.ini")

file(GLOB _files "${LOCALE_DIR}/*.ini")
set(_count 0)
set(_present "")
foreach(_file IN LISTS _files)
  get_filename_component(_name "${_file}" NAME)
  if(_name STREQUAL "en-US.ini")
    continue()
  endif()
  # OBS looks a language up by its exact name, case included
  if(NOT _name MATCHES "^[a-z][a-z][a-z]?-[A-Z][A-Z]\\.ini$")
    message(SEND_ERROR "${_name}: not named like de-DE.ini, so OBS would not find it")
  endif()
  string(REGEX REPLACE "\\.ini$" "" _locale "${_name}")
  list(APPEND _present "${_locale}")
  math(EXPR _count "${_count} + 1")
  check_encoding("${_file}")
  read_locale("${_file}" TR)
  foreach(_key IN LISTS EN_KEYS)
    if(NOT _key IN_LIST TR_KEYS)
      message(SEND_ERROR "${_name}: missing key ${_key}")
      continue()
    endif()
    if(_key IN_LIST _format_keys)
      conversions("${EN_${_key}}" _english)
      conversions("${TR_${_key}}" _translated)
      if(NOT _english STREQUAL _translated)
        list(JOIN _english " " _english)
        list(JOIN _translated " " _translated)
        message(SEND_ERROR "${_name}: ${_key} has conversions \"${_translated}\", English has \"${_english}\"")
      endif()
    endif()
  endforeach()
  foreach(_key IN LISTS TR_KEYS)
    if(NOT _key IN_LIST EN_KEYS)
      message(SEND_ERROR "${_name}: extra key ${_key}, not in en-US.ini")
    endif()
    unset(TR_${_key})
  endforeach()
endforeach()

if(REQUIRED_LOCALES)
  string(REPLACE "," ";" _required "${REQUIRED_LOCALES}")
  foreach(_locale IN LISTS _required)
    if(NOT _locale IN_LIST _present)
      message(SEND_ERROR "${_locale}.ini: missing, but required (R-I18N-3)")
    endif()
  endforeach()
endif()

foreach(_key IN LISTS _format_keys)
  if(NOT _key IN_LIST EN_KEYS)
    message(SEND_ERROR "en-US.ini: format key ${_key} is missing")
    continue()
  endif()
  # a stray "%" in the English itself would make both sides "BAD" and look equal
  conversions("${EN_${_key}}" _english)
  if("BAD" IN_LIST _english)
    message(SEND_ERROR "en-US.ini: format key ${_key} has a \"%\" that starts no conversion")
  endif()
endforeach()

list(LENGTH EN_KEYS _keys)
list(REMOVE_DUPLICATES _format_keys)
list(LENGTH _format_keys _formats)
message(STATUS "locale files: ${_count} translations checked against ${_keys} keys (${_formats} printf formats)")
