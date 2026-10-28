# visual-sidechain CTest suite. Tests link OBS::libobs (util layer only) and run headless.
# Copyright (C) 2026 Jeffrey A. Brick
# SPDX-License-Identifier: GPL-2.0-or-later

# Directories holding libobs and its dependency DLLs, put on the test PATH so nothing is copied and nothing is taken
# from an installed OBS or a developer PATH (REQUIREMENTS R-TST-1).
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
message(STATUS "visual-sidechain tests: DLL search dirs: ${_test_dll_dirs}")

# One executable per suite: links only OBS::libobs and runs with the DLL directories above on its PATH
function(visual_sidechain_add_test name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_compile_definitions(
    ${name}
    PRIVATE
      "VS_TEST_FIXTURES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\""
      "VS_SHADERS_DIR=\"${CMAKE_SOURCE_DIR}/data/shaders\""
  )
  target_link_libraries(${name} PRIVATE OBS::libobs)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT_MODIFICATION "${_test_environment}")
endfunction()

set(_core "${CMAKE_SOURCE_DIR}/src/core")
set(
  _analyzer_srcs
  "${_core}/analyzer.c"
  "${_core}/fader-bridge.c"
  "${_core}/onset.c"
  "${_core}/envelope.c"
  "${_core}/spectrum.c"
  "${_core}/fft.c"
  "${_core}/audio-ring.c"
)
visual_sidechain_add_test(test-audio-ring test-audio-ring.c "${_core}/audio-ring.c")
visual_sidechain_add_test(test-fft test-fft.c "${_core}/fft.c")
visual_sidechain_add_test(test-spectrum test-spectrum.c "${_core}/spectrum.c" "${_core}/fft.c")
visual_sidechain_add_test(test-envelope test-envelope.c "${_core}/envelope.c")
visual_sidechain_add_test(test-onset test-onset.c "${_core}/onset.c")
visual_sidechain_add_test(test-analyzer test-analyzer.c ${_analyzer_srcs})
visual_sidechain_add_test(test-modulation test-modulation.c "${_core}/modulation.c")
visual_sidechain_add_test(test-modulator-baseline test-modulator-baseline.c "${_core}/baseline.c")
visual_sidechain_add_test(test-shader-parser test-shader-parser.c "${_core}/shader-parse.c")
visual_sidechain_add_test(
  test-fader-bridge
  test-fader-bridge.c
  "${_core}/modulation.c"
  ${_analyzer_srcs}
)
visual_sidechain_add_test(
  test-modulator-properties
  test-modulator-properties.c
  "${CMAKE_SOURCE_DIR}/src/obs/modulator-filter.c"
  "${CMAKE_SOURCE_DIR}/src/obs/audio-tap.c"
  "${_core}/modulation.c"
  "${_core}/baseline.c"
  ${_analyzer_srcs}
)
target_link_libraries(test-modulator-properties PRIVATE plugin-support)

# Every locale key used in src/ is in en-US.ini (R-TST-4); the second test proves the check fails on a missing key
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
