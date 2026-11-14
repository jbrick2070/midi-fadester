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
  "${_core}/flash-clock.c"
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
visual_sidechain_add_test(test-onset test-onset.c "${_core}/onset.c" "${_core}/flash-clock.c")
visual_sidechain_add_test(test-analyzer test-analyzer.c ${_analyzer_srcs})
visual_sidechain_add_test(test-modulation test-modulation.c "${_core}/modulation.c" "${_core}/flash-clock.c")
visual_sidechain_add_test(test-modulator-baseline test-modulator-baseline.c "${_core}/baseline.c")
visual_sidechain_add_test(test-shader-parser test-shader-parser.c "${_core}/shader-parse.c")
visual_sidechain_add_test(
  test-text-format
  test-text-format.c
  "${_core}/text-format.c"
  "${CMAKE_SOURCE_DIR}/src/obs/text.c"
)
visual_sidechain_add_test(
  test-fader-bridge
  test-fader-bridge.c
  "${_core}/modulation.c"
  ${_analyzer_srcs}
)
visual_sidechain_add_test(
  test-vendor-api
  test-vendor-api.c
  "${CMAKE_SOURCE_DIR}/src/obs/vendor-api.c"
  ${_analyzer_srcs}
)
target_link_libraries(test-vendor-api PRIVATE plugin-support)
visual_sidechain_add_test(
  test-modulator-properties
  test-modulator-properties.c
  "${CMAKE_SOURCE_DIR}/src/obs/modulator-filter.c"
  "${CMAKE_SOURCE_DIR}/src/obs/audio-tap.c"
  "${CMAKE_SOURCE_DIR}/src/obs/text.c"
  "${_core}/modulation.c"
  "${_core}/baseline.c"
  "${_core}/text-format.c"
  ${_analyzer_srcs}
)
target_link_libraries(test-modulator-properties PRIVATE plugin-support)

# Tubester's core (docs/tubester/REQUIREMENTS.md): pure C, no OBS running
set(_tubester "${CMAKE_SOURCE_DIR}/src/tubester")
visual_sidechain_add_test(test-tubester-core test-tubester-core.c "${_tubester}/tape-url.c" "${_tubester}/speed.c")
visual_sidechain_add_test(
  test-tubester-link
  test-tubester-link.c
  "${_tubester}/deck-link.c"
  "${_tubester}/command.c"
  "${_tubester}/speed.c"
)
# The report listener (R-LNK-2): request parsing and the token source alone, then the listener over a real loopback
# socket. Both are Windows-only, like the plugin (R-PLAT-9).
visual_sidechain_add_test(test-tubester-http test-tubester-http.c "${_tubester}/http-parse.c" "${_tubester}/random.c")
target_link_libraries(test-tubester-http PRIVATE bcrypt)
visual_sidechain_add_test(
  test-tubester-listener
  test-tubester-listener.c
  "${_tubester}/listener.c"
  "${_tubester}/http-parse.c"
  "${_tubester}/deck-link.c"
)
target_link_libraries(test-tubester-listener PRIVATE ws2_32)

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
add_test(
  NAME check-locale-keys-fails-on-changed-english
  COMMAND
    "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
    "-DEXTRA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/locale-format-mismatch.c" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/check-locale-keys.cmake"
)
set_tests_properties(
  check-locale-keys-fails-on-changed-english
  PROPERTIES PASS_REGULAR_EXPRESSION "Shader\.Error\.FileNotFound: the English in .*locale-format-mismatch\.c"
)

# Every translation matches en-US.ini (R-I18N-4): the real files with the eleven required languages, a file full of
# awkward characters must pass, and each kind of fault must be caught (check-locale-files-bad.cmake)
add_test(
  NAME check-locale-files
  COMMAND
    "${CMAKE_COMMAND}" "-DLOCALE_DIR=${CMAKE_SOURCE_DIR}/data/locale" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
    # the eleven languages of R-I18N-3: a missing file fails
    "-DREQUIRED_LOCALES=zh-CN,es-ES,pt-BR,ru-RU,ja-JP,de-DE,fr-FR,ko-KR,zh-TW,tr-TR,nl-NL" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/check-locale-files.cmake"
)
add_test(
  NAME check-locale-files-passes-a-good-file
  COMMAND
    "${CMAKE_COMMAND}" "-DLOCALE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/locale-good" "-DFORMAT_KEYS=Greeting" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/check-locale-files.cmake"
)
foreach(
  _case
  IN
  ITEMS
    "missing-key|de-DE\.ini: missing key Gone"
    "extra-key|de-DE\.ini: extra key Extra"
    "repeated-key|de-DE\.ini: key Extra appears more than once"
    "lost-conversion|de-DE\\.ini: Greeting has conversions \"%s\", English has \"%s %\\.0f\""
    "swapped-conversions|de-DE\\.ini: Order has conversions \"%\\.4g %s %s\", English has \"%s %s %\\.4g\""
    "stray-percent|de-DE\\.ini: Stray has conversions \"%s BAD\", English has \"%s\""
    "stray-percent-in-english|en-US\\.ini: format key Broken has a \"%\" that starts no conversion"
    "stray-quote|de-DE\.ini: Plain has a quote that is not escaped"
    "bom|fr-FR\.ini: starts with a BOM"
    "invalid-utf8|fr-FR\.ini: not valid UTF-8"
    "file-name|german\.ini: not named like de-DE\.ini"
    "missing-language|ja-JP\.ini: missing, but required"
)
  string(REPLACE "|" ";" _parts "${_case}")
  list(GET _parts 0 _name)
  list(GET _parts 1 _expected)
  add_test(
    NAME check-locale-files-catches-${_name}
    COMMAND
      "${CMAKE_COMMAND}" "-DFIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/locale-bad"
      "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/locale-bad-${_name}" -P
      "${CMAKE_CURRENT_SOURCE_DIR}/check-locale-files-bad.cmake"
  )
  set_tests_properties(check-locale-files-catches-${_name} PROPERTIES PASS_REGULAR_EXPRESSION "${_expected}")
endforeach()

# The built-in shaders' parameter texts are in en-US.ini word for word (R-I18N-1), so the translations follow them; the
# second test proves a missing, a changed and an extra text are each caught
add_test(
  NAME check-shader-locale
  COMMAND
    "${CMAKE_COMMAND}" "-DSHADER_DIR=${CMAKE_SOURCE_DIR}/data/shaders"
    "-DLOCALE_FILE=${CMAKE_SOURCE_DIR}/data/locale/en-US.ini" -P "${CMAKE_CURRENT_SOURCE_DIR}/check-shader-locale.cmake"
)
add_test(
  NAME check-shader-locale-catches-faults
  COMMAND
    "${CMAKE_COMMAND}" "-DSHADER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/shader-locale-bad"
    "-DLOCALE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/shader-locale-bad/en-US.ini" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/check-shader-locale.cmake"
)
set_tests_properties(
  check-shader-locale-catches-faults
  PROPERTIES PASS_REGULAR_EXPRESSION "shader locale: 1 missing, 1 different, 1 extra"
)

# Built-in shaders may only use raw transient uniforms for allow-listed spatial geometry, never colour/luminance (P0-1)
add_test(
  NAME check-shader-flash-safety
  COMMAND
    "${CMAKE_COMMAND}" "-DSHADER_DIR=${CMAKE_SOURCE_DIR}/data/shaders" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/check-shader-flash-safety.cmake"
)
add_test(
  NAME check-shader-flash-safety-catches-faults
  COMMAND
    "${CMAKE_COMMAND}" "-DSHADER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/shader-flash-bad" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/check-shader-flash-safety.cmake"
)
set_tests_properties(
  check-shader-flash-safety-catches-faults
  PROPERTIES PASS_REGULAR_EXPRESSION "shader flash safety: .*ungated-flash\\.shader.*unguarded raw transient"
)
