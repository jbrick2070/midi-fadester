# Midi Fadester CMake build definition
# Copyright (C) 2026 Jeffrey A. Brick
# SPDX-License-Identifier: GPL-2.0-or-later

cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(MIDI_FADESTER_BUILD_TESTS "Build the CTest suite" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 COMPONENTS Widgets Core Gui Svg REQUIRED)

target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
)

# MIDI input: WinMM on Windows, CoreMIDI on macOS, both behind src/midi/winmm-device.h.
if(OS_WINDOWS)
  set(_midi_backend "${CMAKE_CURRENT_SOURCE_DIR}/src/midi/winmm-device.c")
  set(_midi_libraries winmm)
elseif(OS_MACOS)
  set(_midi_backend "${CMAKE_CURRENT_SOURCE_DIR}/src/midi/coremidi-device.c")
  find_library(COREMIDI_FRAMEWORK CoreMIDI REQUIRED)
  find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
  set(_midi_libraries "${COREMIDI_FRAMEWORK}" "${COREFOUNDATION_FRAMEWORK}")
else()
  message(FATAL_ERROR "Midi Fadester requires Windows or macOS.")
endif()
target_sources(${CMAKE_PROJECT_NAME} PRIVATE "${_midi_backend}")
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${_midi_libraries})

set_target_properties(
  ${CMAKE_PROJECT_NAME}
  PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE QT_NO_KEYWORDS)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/core/midi-types.h
    src/core/midi-queue.h
    src/core/midi-queue.c
    src/core/midi-parser.h
    src/core/midi-parser.c
    src/core/smoother.h
    src/core/smoother.c
    src/core/takeover.h
    src/core/takeover.c
    src/core/mixer-math.h
    src/core/mixer-math.c
    src/core/apc40-profile.h
    src/core/apc40-profile.c
    src/core/tbar-logic.h
    src/core/tbar-logic.c
    src/core/vj-pads.h
    src/core/vj-pads.c
    src/core/apc40-palette.h
    src/core/apc40-palette.c
    src/core/filter-math.h
    src/core/filter-math.c
    src/core/ui-sentences.h
    src/core/ui-sentences.c
    src/core/slot-messages.h
    src/core/slot-messages.c
    src/core/mfd-text.h
    src/core/mfd-text.c
    src/midi/winmm-device.h
    src/obs/mfd-layer-filter.h
    src/obs/mfd-layer-filter.c
    src/obs/mfd-engine.h
    src/obs/mfd-engine.c
    src/obs/mfd-blackout.c
    src/obs/mfd-blackout.h
    src/obs/mfd-tbar.h
    src/obs/mfd-tbar.cpp
    src/obs/obs-websocket-api.h
    src/obs/mfd-vendor-api.h
    src/obs/mfd-vendor-api.c
    src/ui/mfd-text-qt.hpp
    src/ui/mfd-text-qt.cpp
    src/ui/mfd-svg-canvas.hpp
    src/ui/mfd-controller-svg.hpp
    src/ui/mfd-controller-svg.cpp
    src/ui/mfd-setup-sheet.hpp
    src/ui/mfd-setup-sheet.cpp
    src/ui/mfd-settings-dialog.hpp
    src/ui/mfd-settings-dialog.cpp
    src/ui/mfd-desk-dock.hpp
    src/ui/mfd-desk-dock.cpp
    # Visual Sidechain (part of Midi Fadester since 0.9.0)
    src/vs/vs-module.h
    src/vs/vs-module.c
    src/vs/vs-conflict.c
    src/vs/core/audio-ring.h
    src/vs/core/audio-ring.c
    src/vs/core/fft.h
    src/vs/core/fft.c
    src/vs/core/spectrum.h
    src/vs/core/spectrum.c
    src/vs/core/envelope.h
    src/vs/core/envelope.c
    src/vs/core/flash-clock.h
    src/vs/core/flash-clock.c
    src/vs/core/tempo.h
    src/vs/core/tempo.c
    src/vs/core/onset.h
    src/vs/core/onset.c
    src/vs/core/analyzer.h
    src/vs/core/analyzer.c
    src/vs/core/fader-bridge.h
    src/vs/core/fader-bridge.c
    src/vs/core/modulation.h
    src/vs/core/modulation.c
    src/vs/core/baseline.h
    src/vs/core/baseline.c
    src/vs/core/shader-parse.h
    src/vs/core/shader-parse.c
    src/vs/core/text-format.h
    src/vs/core/text-format.c
    src/vs/obs/audio-tap.h
    src/vs/obs/audio-tap.c
    src/vs/obs/modulator-filter.h
    src/vs/obs/modulator-filter.c
    src/vs/obs/shader-filter.h
    src/vs/obs/shader-filter.c
    src/vs/obs/property-status.h
    src/vs/obs/text.h
    src/vs/obs/text.c
    src/vs/obs/vendor-api.h
    src/vs/obs/vendor-api.c
    src/vs/tubester/tape-url.h
    src/vs/tubester/tape-url.c
    src/vs/tubester/speed.h
    src/vs/tubester/speed.c
    src/vs/tubester/deck-link.h
    src/vs/tubester/deck-link.c
    src/vs/tubester/command.h
    src/vs/tubester/command.c
    src/vs/tubester/http-parse.h
    src/vs/tubester/http-parse.c
    src/vs/tubester/random.h
    src/vs/tubester/random.c
    src/vs/tubester/listener.h
    src/vs/tubester/listener.c
    src/vs/tubester/socket-compat.h
    src/vs/tubester/deck-filter.h
    src/vs/tubester/deck-filter.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE src src/vs)
# Visual Sidechain's Tape Deck report listener and token source: Winsock and BCrypt on Windows; POSIX sockets and
# arc4random_buf on macOS come with the system library
if(OS_WINDOWS)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ws2_32 bcrypt)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(MIDI_FADESTER_BUILD_TESTS)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE "$<$<NOT:$<CONFIG:Release>>:MFD_ENABLE_SELF_TEST=1>")
  enable_testing()
  add_subdirectory(tests)
endif()
