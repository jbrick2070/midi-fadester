# visual-sidechain
# Copyright (C) 2026 Jeffrey A. Brick
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Fails when en-US.ini's ShaderParam texts do not match the built-in shaders (REQUIREMENTS R-I18N-1): every label (or
# name), tooltip and option_N annotation of a uniform in SHADER_DIR/<file>.shader needs the key
# ShaderParam.<file>.<uniform> (plus ".Tooltip" or ".Option<N>") with exactly the shader's text, and no ShaderParam key
# may name a text no shader has. The translations follow en-US.ini's keys through check-locale-files. Run with:
#   cmake -DSHADER_DIR=<folder> -DLOCALE_FILE=<en-US.ini> -P check-shader-locale.cmake
# The negative ctest runs it on tests/fixtures/shader-locale-bad to prove each kind of fault is caught.

cmake_minimum_required(VERSION 3.28)

if(NOT SHADER_DIR OR NOT LOCALE_FILE)
  message(FATAL_ERROR "SHADER_DIR and LOCALE_FILE are required")
endif()

# characters CMake lists treat specially stand in for themselves while texts are split; nothing here looks at them
string(ASCII 1 _semicolon)
string(ASCII 2 _open)
string(ASCII 3 _close)
function(protect text out)
  string(REPLACE ";" "${_semicolon}" text "${text}")
  string(REPLACE "[" "${_open}" text "${text}")
  string(REPLACE "]" "${_close}" text "${text}")
  set(${out} "${text}" PARENT_SCOPE)
endfunction()

# the texts the shaders need: key -> text, as EXPECT_<key>, and the keys in EXPECT_KEYS
set(_expect_keys "")
file(GLOB _shaders "${SHADER_DIR}/*.shader")
foreach(_shader IN LISTS _shaders)
  get_filename_component(_base "${_shader}" NAME)
  string(REGEX REPLACE "\\.shader$" "" _base "${_base}")
  file(READ "${_shader}" _text)
  string(REPLACE "\r" "" _text "${_text}")
  protect("${_text}" _text)
  # an annotated uniform: uniform <type> <name> < ... >
  string(REGEX MATCHALL "uniform[ \t]+[A-Za-z0-9_]+[ \t]+[A-Za-z0-9_]+[ \t\n]*<[^>]*>" _blocks "${_text}")
  foreach(_block IN LISTS _blocks)
    string(REGEX REPLACE "^uniform[ \t]+[A-Za-z0-9_]+[ \t]+([A-Za-z0-9_]+).*$" "\\1" _name "${_block}")
    string(REGEX MATCHALL "string[ \t]+(label|name|tooltip|option_[0-7])[ \t]*=[ \t]*\"[^\"]*\"" _notes "${_block}")
    foreach(_note IN LISTS _notes)
      string(REGEX REPLACE "^string[ \t]+([a-z_0-7]+)[ \t]*=.*$" "\\1" _kind "${_note}")
      string(REGEX REPLACE "^[^\"]*\"([^\"]*)\"$" "\\1" _value "${_note}")
      if(_kind STREQUAL "label" OR _kind STREQUAL "name")
        set(_suffix "")
      elseif(_kind STREQUAL "tooltip")
        set(_suffix ".Tooltip")
      else()
        string(REPLACE "option_" ".Option" _suffix "${_kind}")
      endif()
      set(_key "ShaderParam.${_base}.${_name}${_suffix}")
      list(APPEND _expect_keys "${_key}")
      set(EXPECT_${_key} "${_value}")
    endforeach()
  endforeach()
endforeach()

# the ShaderParam texts the locale file has, as OBS reads key="text"
set(_have_keys "")
file(READ "${LOCALE_FILE}" _locale)
string(REPLACE "\r" "" _locale "${_locale}")
protect("${_locale}" _locale)
string(REPLACE "\n" ";" _lines "${_locale}")
foreach(_line IN LISTS _lines)
  if(_line MATCHES "^[ \t]*(ShaderParam\\.[^= \t]+)[ \t]*=[ \t]*\"(.*)\"[ \t]*$")
    list(APPEND _have_keys "${CMAKE_MATCH_1}")
    set(HAVE_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
  endif()
endforeach()

set(_missing 0)
set(_different 0)
set(_extra 0)
foreach(_key IN LISTS _expect_keys)
  if(NOT _key IN_LIST _have_keys)
    message(SEND_ERROR "missing ${_key} (the shader's text: \"${EXPECT_${_key}}\")")
    math(EXPR _missing "${_missing} + 1")
  elseif(NOT HAVE_${_key} STREQUAL EXPECT_${_key})
    message(SEND_ERROR "${_key} differs: the shader has \"${EXPECT_${_key}}\", the locale file \"${HAVE_${_key}}\"")
    math(EXPR _different "${_different} + 1")
  endif()
endforeach()
foreach(_key IN LISTS _have_keys)
  if(NOT _key IN_LIST _expect_keys)
    message(SEND_ERROR "extra ${_key}: no shader has this text")
    math(EXPR _extra "${_extra} + 1")
  endif()
endforeach()

list(LENGTH _shaders _shader_count)
list(LENGTH _expect_keys _key_count)
if(_missing OR _different OR _extra)
  message(FATAL_ERROR "shader locale: ${_missing} missing, ${_different} different, ${_extra} extra")
endif()
message(STATUS "shader locale: ${_key_count} ShaderParam texts match ${_shader_count} shaders")
