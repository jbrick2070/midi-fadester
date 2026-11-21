# CMake Windows build dependencies module
# Copyright (C) 2026 Jeffrey A. Brick
# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

include(buildspec_common)

function(_check_dependencies_windows)
  set(arch ${CMAKE_VS_PLATFORM_NAME})
  set(platform windows-${arch})

  # A dependency folder shared with another checkout, only when asked for (H-15): empty by default, so a fresh clone
  # downloads and checks the versions and hashes in buildspec.json. It used to be one developer's path, taken without
  # a word whenever it existed. Set -DMIDI_FADESTER_SHARED_DEPS=<folder> (or the environment variable of that name).
  set(
    MIDI_FADESTER_SHARED_DEPS
    "$ENV{MIDI_FADESTER_SHARED_DEPS}"
    CACHE PATH
    "A .deps folder of another checkout to build against instead of downloading (must hold the obs-deps of buildspec.json)"
  )
  set(_shared_deps "${MIDI_FADESTER_SHARED_DEPS}")
  if(
    _shared_deps
    AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.deps/cmake/libobsConfig.cmake"
    AND EXISTS "${_shared_deps}/cmake/libobsConfig.cmake"
    AND EXISTS "${_shared_deps}/obs-deps-2026-10-23-${arch}"
    AND EXISTS "${_shared_deps}/obs-deps-qt6-2026-10-23-${arch}"
  )
    message(WARNING "Building against the shared dependency folder ${_shared_deps} (MIDI_FADESTER_SHARED_DEPS)")
    list(
      APPEND
      CMAKE_PREFIX_PATH
      "${_shared_deps}/obs-deps-2026-10-23-${arch}"
      "${_shared_deps}/obs-deps-qt6-2026-10-23-${arch}"
      "${_shared_deps}"
    )
    list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)
    set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} CACHE PATH "CMake prefix search path" FORCE)
    return()
  endif()

  set(dependencies_dir "${CMAKE_CURRENT_SOURCE_DIR}/.deps")
  set(prebuilt_filename "windows-deps-VERSION-ARCH-REVISION.zip")
  set(prebuilt_destination "obs-deps-VERSION-ARCH")
  set(qt6_filename "windows-deps-qt6-VERSION-ARCH-REVISION.zip")
  set(qt6_destination "obs-deps-qt6-VERSION-ARCH")
  set(obs-studio_filename "VERSION.zip")
  set(obs-studio_destination "obs-studio-VERSION")
  set(dependencies_list prebuilt qt6 obs-studio)

  _check_dependencies()
endfunction()

_check_dependencies_windows()
