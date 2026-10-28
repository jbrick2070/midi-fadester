# Midi Fadester CTest suite
# Copyright (C) 2026 Jeffrey A. Brick
# SPDX-License-Identifier: GPL-2.0-or-later

set(_test_dll_dirs "")
foreach(_config IN ITEMS RELEASE RELWITHDEBINFO DEBUG NOCONFIG)
  get_target_property(_libobs_location OBS::libobs IMPORTED_LOCATION_${_config})
  if(_libobs_location)
    cmake_path(GET _libobs_location PARENT_PATH _libobs_dir)
    list(APPEND _test_dll_dirs "${_libobs_dir}")
    break()
  endif()
endforeach()
foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
  if(EXISTS "${_prefix}/share/obs-deps/VERSION" AND EXISTS "${_prefix}/bin")
    list(APPEND _test_dll_dirs "${_prefix}/bin")
  endif()
endforeach()

set(_test_environment "")
foreach(_dir IN LISTS _test_dll_dirs)
  cmake_path(NATIVE_PATH _dir _native_dir)
  list(APPEND _test_environment "PATH=path_list_prepend:${_native_dir}")
endforeach()
message(STATUS "midi-fadester tests: DLL search dirs: ${_test_dll_dirs}")

function(midi_fadester_add_test name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(${name} PRIVATE OBS::libobs)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT_MODIFICATION "${_test_environment}")
endfunction()

set(_core "${CMAKE_SOURCE_DIR}/src/core")
midi_fadester_add_test(test-midi-queue test-midi-queue.c "${_core}/midi-queue.c")
midi_fadester_add_test(test-midi-parser test-midi-parser.c "${_core}/midi-parser.c")
midi_fadester_add_test(test-smoother test-smoother.c "${_core}/smoother.c")
midi_fadester_add_test(test-takeover test-takeover.c "${_core}/takeover.c")
midi_fadester_add_test(test-mixer-math test-mixer-math.c "${_core}/mixer-math.c")
midi_fadester_add_test(test-apc40-profile test-apc40-profile.c "${_core}/apc40-profile.c")
midi_fadester_add_test(test-tbar-logic test-tbar-logic.c "${_core}/tbar-logic.c")
midi_fadester_add_test(test-ui-sentences test-ui-sentences.c "${_core}/ui-sentences.c" "${_core}/takeover.c")

add_executable(
  test-vendor-api
  test-vendor-api.c
  "${CMAKE_SOURCE_DIR}/src/obs/mfd-vendor-api.c"
  "${CMAKE_SOURCE_DIR}/src/obs/mfd-engine.c"
  "${CMAKE_SOURCE_DIR}/src/obs/mfd-layer-filter.c"
  "${CMAKE_SOURCE_DIR}/src/midi/winmm-device.c"
  "${_core}/midi-queue.c"
  "${_core}/midi-parser.c"
  "${_core}/smoother.c"
  "${_core}/takeover.c"
  "${_core}/mixer-math.c"
  "${_core}/apc40-profile.c"
  "${_core}/tbar-logic.c"
)
target_include_directories(test-vendor-api PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(test-vendor-api PRIVATE OBS::libobs OBS::obs-frontend-api plugin-support winmm)
add_test(NAME test-vendor-api COMMAND test-vendor-api)
set_tests_properties(test-vendor-api PROPERTIES ENVIRONMENT_MODIFICATION "${_test_environment}")

add_executable(midi-send midi-send.c)
target_link_libraries(midi-send PRIVATE winmm)
add_test(NAME test-midi-send COMMAND midi-send --self-test)
set_tests_properties(test-midi-send PROPERTIES ENVIRONMENT_MODIFICATION "${_test_environment}")

add_executable(test-shader-syntax test-shader-syntax.c)
add_test(
  NAME test-shader-syntax
  COMMAND
    test-shader-syntax "${CMAKE_SOURCE_DIR}/data/effects/mfd_layer_mixer.effect"
    "${CMAKE_SOURCE_DIR}/src/ui/mfd-settings-dialog.cpp"
)
set_tests_properties(test-shader-syntax PROPERTIES ENVIRONMENT_MODIFICATION "${_test_environment}")

add_test(
  NAME check-locale-keys
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/check-locale-keys.cmake"
)
add_test(
  NAME check-locale-keys-fails-on-missing-key
  COMMAND
    "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
    "-DEXTRA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/locale-missing-key.c" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/check-locale-keys.cmake"
)
set_tests_properties(
  check-locale-keys-fails-on-missing-key
  PROPERTIES PASS_REGULAR_EXPRESSION "LocaleKeyThatIsNotInTheLocaleFile \\(.*locale-missing-key\\.c\\)"
)
