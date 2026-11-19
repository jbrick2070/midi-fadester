# visual-sidechain
# Copyright (C) 2026 Jeffrey A. Brick
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Static portability check for built-in shaders (R-PLAT-9), run on every platform:
# OBS on macOS turns each shader into GLSL (libobs-opengl) or Metal Shading Language (libobs-metal). Both translators
# copy unknown tokens through, so two HLSL forms that Direct3D accepts reach the Mac's compiler and are refused:
#   - C-style casts such as (float)i: GLSL has none. Write float(i), which HLSL, GLSL and Metal all accept.
#   - Attributes such as [unroll] or [branch] before a statement: GLSL has none. Leave them out.
# test-shader-gpu (macOS CI) compiles every shader for real; this check catches the two known forms on Windows too.

cmake_minimum_required(VERSION 3.28)

if(NOT SHADER_DIR)
  message(FATAL_ERROR "SHADER_DIR is required")
endif()

file(GLOB _shaders LIST_DIRECTORIES false "${SHADER_DIR}/*.shader")
list(SORT _shaders)
if(NOT _shaders)
  message(FATAL_ERROR "no .shader files in ${SHADER_DIR}")
endif()

set(_types "(float|half|double|int|uint|bool|min16float|min16int)[1-4]?(x[1-4])?")
set(_cast_pat "\\([ \t]*${_types}[ \t]*\\)[ \t]*[A-Za-z_0-9(.]")
set(_attr_pat "\\[(unroll|loop|branch|flatten|fastopt|call|forcecase|allow_uav_condition)[^]]*\\]")

set(_faults "")
foreach(_shader IN LISTS _shaders)
  get_filename_component(_fname "${_shader}" NAME)
  file(READ "${_shader}" _raw)

  # Strip /* ... */ block comments, // ... line comments, and "..." string literals
  string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" " " _code "${_raw}")
  string(REGEX REPLACE "//[^\r\n]*" " " _code "${_code}")
  string(REGEX REPLACE "\"([^\"\\\\]|\\\\.)*\"" "\"\"" _code "${_code}")

  string(REGEX MATCHALL "${_cast_pat}" _casts "${_code}")
  foreach(_m IN LISTS _casts)
    list(APPEND _faults "${_fname}: C-style cast '${_m}' (write float(x), not (float)x)")
  endforeach()
  string(REGEX MATCHALL "${_attr_pat}" _attrs "${_code}")
  foreach(_m IN LISTS _attrs)
    list(APPEND _faults "${_fname}: HLSL attribute '${_m}' (GLSL has none, so leave it out)")
  endforeach()
endforeach()

list(LENGTH _shaders _count)
if(_faults)
  list(JOIN _faults "\n  " _report)
  message(FATAL_ERROR "shader portability: the Mac's GLSL and Metal compilers refuse:\n  ${_report}")
endif()
message(STATUS "shader portability: ${_count} shaders, no C-style casts or HLSL attributes")
