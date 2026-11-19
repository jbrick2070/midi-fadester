# visual-sidechain
# Copyright (C) 2026 Jeffrey A. Brick
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Enforces P1-7 suite naming invariants across data/locale/*.ini:
#   - No locale file may contain the legacy product name "Stream Deck Fadester" or "(Midi / " / "(Midi & ".
#   - en-US.ini must qualify Restore as "Restore visuals" in Hotkey.Restore, Panic.Active, and Shader.PanicHint,
#     and spell Tape Deck cue as "Cue to start" in Tubester.CueToStart and Tubester.Hotkey.CueToStart.

cmake_minimum_required(VERSION 3.28)

if(NOT LOCALE_DIR)
  message(FATAL_ERROR "LOCALE_DIR is required")
endif()

file(GLOB _files "${LOCALE_DIR}/*.ini")
if(NOT _files)
  message(FATAL_ERROR "product naming: no .ini files in ${LOCALE_DIR}")
endif()

set(_errors "")

foreach(_file IN LISTS _files)
  get_filename_component(_name "${_file}" NAME)
  file(READ "${_file}" _text)
  if(_text MATCHES "Stream Deck Fadester")
    list(APPEND _errors "${_name}: contains legacy product name 'Stream Deck Fadester'")
  endif()
  if(_text MATCHES "\\(Midi [\\/&]")
    list(APPEND _errors "${_name}: contains bare '(Midi /' or '(Midi &' instead of 'Midi Fadester'")
  endif()
  if(_name STREQUAL "en-US.ini")
    if(NOT _text MATCHES "(^|\n)Hotkey\\.Restore=\"Visual Sidechain: Restore visuals \\(effects on again\\)\"")
      list(APPEND _errors "en-US.ini: Hotkey.Restore must say 'Restore visuals'")
    endif()
    if(NOT _text MATCHES "(^|\n)Panic\\.Active=\"[^\n]*Restore visuals[^\n]*\"")
      list(APPEND _errors "en-US.ini: Panic.Active must say 'Restore visuals'")
    endif()
    if(NOT _text MATCHES "(^|\n)Shader\\.PanicHint=\"[^\n]*Restore visuals[^\n]*\"")
      list(APPEND _errors "en-US.ini: Shader.PanicHint must say 'Restore visuals'")
    endif()
    if(NOT _text MATCHES "(^|\n)Tubester\\.CueToStart=\"Cue to start\"")
      list(APPEND _errors "en-US.ini: Tubester.CueToStart must say 'Cue to start'")
    endif()
    if(NOT _text MATCHES "(^|\n)Tubester\\.Hotkey\\.CueToStart=\"Tape Deck: Cue to start\"")
      list(APPEND _errors "en-US.ini: Tubester.Hotkey.CueToStart must say 'Tape Deck: Cue to start'")
    endif()
  endif()
endforeach()

if(_errors)
  list(JOIN _errors "\n  " _joined)
  message(FATAL_ERROR "product naming check failed:\n  ${_joined}")
endif()

list(LENGTH _files _count)
message(STATUS "product naming: ${_count} locale file(s) clean")
