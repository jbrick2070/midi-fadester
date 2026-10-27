# Midi Fadester CMake build definition
# Copyright (C) 2026 Jeffrey A. Brick
# SPDX-License-Identifier: GPL-2.0-or-later

cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)
option(MIDI_FADESTER_BUILD_TESTS "Build the CTest suite" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs winmm)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

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
    src/midi/winmm-device.h
    src/midi/winmm-device.c
    src/obs/mfd-layer-filter.h
    src/obs/mfd-layer-filter.c
    src/obs/mfd-engine.h
    src/obs/mfd-engine.c
    src/obs/mfd-tbar.h
    src/obs/mfd-tbar.cpp
    src/obs/obs-websocket-api.h
    src/obs/mfd-vendor-api.h
    src/obs/mfd-vendor-api.c
    src/ui/mfd-settings-dialog.hpp
    src/ui/mfd-settings-dialog.cpp
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE src)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(MIDI_FADESTER_BUILD_TESTS)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE "$<$<NOT:$<CONFIG:Release>>:MFD_ENABLE_SELF_TEST=1>")
  enable_testing()
  add_subdirectory(tests)
endif()
