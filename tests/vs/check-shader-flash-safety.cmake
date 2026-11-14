# visual-sidechain
# Copyright (C) 2026 Jeffrey A. Brick
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Static photosensitivity check for built-in shaders (P0-1):
# No built-in shader may drive whole-frame colour, luminance, saturation, or inversion from a raw transient uniform
# (audio_kick, audio_snare, audio_spectral_flux, audio_flux). Every raw transient occurrence must match an explicit
# spatial/UV geometry allow-list entry, and no allow-listed geometry variable may multiply or offset final RGB/luma.

cmake_minimum_required(VERSION 3.28)

if(NOT SHADER_DIR)
  message(FATAL_ERROR "SHADER_DIR is required")
endif()

file(GLOB _shaders LIST_DIRECTORIES false "${SHADER_DIR}/*.shader")
list(SORT _shaders)
if(NOT _shaders)
  message(FATAL_ERROR "no .shader files in ${SHADER_DIR}")
endif()

# Explicit geometry allow-list: "<shader-file>|<assigned-target>"
set(
  _geometry_allowlist
  "fft-chromatic-prism-bloom.shader|disp"
  "fft-feedback-trails.shader|geom_energy"
  "fft-hex-pulse-lattice.shader|kick"
  "fft-kaleidoscope.shader|a"
  "fft-kaleidoscope.shader|shifted_r"
  "fft-mirror-split.shader|kick_offset"
  "fft-polygon-tunnel.shader|kick"
  "fft-visualizer-overlay.shader|ring_base"
  "fft-wave-warp-glitch.shader|transient_gate"
  "fft-zoom-pulse.shader|pulse"
)

string(ASCII 1 _semicolon_sub)
string(ASCII 2 _bracket_l_sub)
string(ASCII 3 _bracket_r_sub)
set(_colour_pat "(return[ \t]+float4|\\.rgb[ \t]*\\*|bloom_drive|aura_mask|vig[ \t]*=|decay[ \t]*=|flash_amt|tint)")

set(_faults "")

foreach(_shader IN LISTS _shaders)
  get_filename_component(_fname "${_shader}" NAME)
  file(READ "${_shader}" _raw)

  # Strip /* ... */ block comments, // ... line comments, and "..." string literals
  string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" " " _code "${_raw}")
  string(REGEX REPLACE "//[^\r\n]*" " " _code "${_code}")
  string(REGEX REPLACE "\"([^\"\\\\]|\\\\.)*\"" "\"\"" _code "${_code}")

  string(REPLACE ";" "${_semicolon_sub}" _lines_escaped "${_code}")
  string(REPLACE "[" "${_bracket_l_sub}" _lines_escaped "${_lines_escaped}")
  string(REPLACE "]" "${_bracket_r_sub}" _lines_escaped "${_lines_escaped}")
  string(REPLACE "\r" "" _lines_escaped "${_lines_escaped}")
  string(REPLACE "\n" ";" _lines "${_lines_escaped}")

  set(_shader_geom_vars "")
  set(_lineno 0)

  foreach(_escaped_line IN LISTS _lines)
    math(EXPR _lineno "${_lineno} + 1")
    string(REPLACE "${_semicolon_sub}" ";" _line "${_escaped_line}")
    string(REPLACE "${_bracket_l_sub}" "[" _line "${_line}")
    string(REPLACE "${_bracket_r_sub}" "]" _line "${_line}")

    set(_padded " ${_line} ")
    if(_padded MATCHES "[^A-Za-z0-9_](audio_kick|audio_snare|audio_spectral_flux|audio_flux)[^A-Za-z0-9_]")
      set(_raw_uniform "${CMAKE_MATCH_1}")
      if(_line MATCHES "^[ \t]*(float[234]?[ \t]+)?([A-Za-z_][A-Za-z0-9_]*)[ \t]*\\+?=")
        set(_lhs "${CMAKE_MATCH_2}")
      else()
        set(_lhs "<expr>")
      endif()

      set(_entry "${_fname}|${_lhs}")
      if(NOT _entry IN_LIST _geometry_allowlist)
        list(APPEND _faults "${_fname}:${_lineno}: unguarded raw transient ${_raw_uniform} in '${_lhs}'")
      else()
        list(APPEND _shader_geom_vars "${_lhs}")
      endif()
    endif()
  endforeach()

  # Verify no allow-listed geometry variable leaks into colour/luminance/vignette/decay/return RGB expressions
  list(REMOVE_DUPLICATES _shader_geom_vars)
  foreach(_gvar IN LISTS _shader_geom_vars)
    set(_lineno 0)
    foreach(_escaped_line IN LISTS _lines)
      math(EXPR _lineno "${_lineno} + 1")
      string(REPLACE "${_semicolon_sub}" ";" _line "${_escaped_line}")
      string(REPLACE "${_bracket_l_sub}" "[" _line "${_line}")
      string(REPLACE "${_bracket_r_sub}" "]" _line "${_line}")
      set(_padded " ${_line} ")
      if(_padded MATCHES "[^A-Za-z0-9_]${_gvar}[^A-Za-z0-9_]" AND _line MATCHES "${_colour_pat}")
        set(_msg "${_fname}:${_lineno}: unguarded raw transient geometry var '${_gvar}' in colour/luminance term")
        list(APPEND _faults "${_msg}")
      endif()
    endforeach()
  endforeach()
endforeach()

if(_faults)
  list(LENGTH _faults _nfaults)
  list(JOIN _faults "\n  " _report)
  message(FATAL_ERROR "shader flash safety: ${_nfaults} fault(s):\n  ${_report}")
endif()

list(LENGTH _shaders _nshaders)
message(STATUS "shader flash safety: ${_nshaders} shaders keep colour/luminance on *_safe or vs_flash_envelope")
