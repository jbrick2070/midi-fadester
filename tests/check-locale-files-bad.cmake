# Midi Fadester
# Copyright (C) 2026 Jeffrey A. Brick
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Ported from obs-playlister. The negative run of check-locale-files.cmake (R-I18N-5): the faulty translations of tests/fixtures/locale-bad, plus
# two files written here, so no such file is stored in the repository (R-STD-1): one with a BOM and an invalid byte,
# and one named so OBS would not find it. Run with:
#   cmake -DFIXTURE_DIR=<tests/fixtures/locale-bad> -DWORK_DIR=<build folder> -P check-locale-files-bad.cmake

cmake_minimum_required(VERSION 3.28)

if(NOT FIXTURE_DIR OR NOT WORK_DIR)
  message(FATAL_ERROR "FIXTURE_DIR and WORK_DIR are required")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(COPY "${FIXTURE_DIR}/" DESTINATION "${WORK_DIR}")
string(ASCII 239 187 191 _bom)
string(ASCII 255 _invalid)
file(WRITE "${WORK_DIR}/fr-FR.ini" "${_bom}Greeting=\"Bonjour %1, %2 fichiers\"\nPlain=\"texte ${_invalid} brut\"\n")
file(WRITE "${WORK_DIR}/german.ini" "Greeting=\"Hallo %1, %2 Dateien\"\nPlain=\"Text\"\nGone=\"da\"\n")
set(LOCALE_DIR "${WORK_DIR}")
include("${CMAKE_CURRENT_LIST_DIR}/check-locale-files.cmake")
