# Visual Sidechain's CTest suite (part of Midi Fadester since 0.9.0; tests and targets are named vs-*). Tests link OBS::libobs (util layer only) and run headless.
# Copyright (C) 2026 Jeffrey A. Brick
# SPDX-License-Identifier: GPL-2.0-or-later

# The analyzer test makes its own WAV files (sine, kick and snare patterns) and reads them back. They are written
# into the build tree (H-12): in the source tree a run rewrote tracked files, and on a platform whose math library
# rounds one sample differently that made the checkout "dirty".
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/wav-fixtures")

if(OS_MACOS)
  # Test executables live outside OBS.app and find libobs.framework and the obs-deps dylibs in .deps at run time.
  set(CMAKE_SKIP_RPATH FALSE)
  set(CMAKE_XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "")
  set(CMAKE_BUILD_RPATH "${CMAKE_SOURCE_DIR}/.deps/Frameworks;${CMAKE_SOURCE_DIR}/.deps/lib")
  foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
    list(APPEND CMAKE_BUILD_RPATH "${_prefix}/lib")
  endforeach()
endif()

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
message(STATUS "Visual Sidechain tests: DLL search dirs: ${_test_dll_dirs}")

# One executable per suite: links only OBS::libobs and runs with the DLL directories above on its PATH
function(visual_sidechain_add_test short_name)
  set(name "vs-${short_name}")
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/src/vs" "${CMAKE_SOURCE_DIR}/src")
  target_compile_definitions(
    ${name}
    PRIVATE
      "VS_TEST_FIXTURES_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/wav-fixtures\""
      "VS_SHADERS_DIR=\"${CMAKE_SOURCE_DIR}/data/shaders\""
  )
  target_link_libraries(${name} PRIVATE OBS::libobs)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT_MODIFICATION "${_test_environment}")
endfunction()

set(_core "${CMAKE_SOURCE_DIR}/src/vs/core")
set(
  _analyzer_srcs
  "${_core}/analyzer.c"
  "${_core}/fader-bridge.c"
  "${_core}/flash-clock.c"
  "${_core}/tempo.c"
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
visual_sidechain_add_test(test-onset test-onset.c "${_core}/onset.c" "${_core}/tempo.c" "${_core}/flash-clock.c")
visual_sidechain_add_test(test-analyzer test-analyzer.c ${_analyzer_srcs})
visual_sidechain_add_test(test-modulation test-modulation.c "${_core}/modulation.c" "${_core}/flash-clock.c")
visual_sidechain_add_test(test-modulator-baseline test-modulator-baseline.c "${_core}/baseline.c")
visual_sidechain_add_test(test-shader-parser test-shader-parser.c "${_core}/shader-parse.c")
visual_sidechain_add_test(
  test-text-format
  test-text-format.c
  "${_core}/text-format.c"
  "${CMAKE_SOURCE_DIR}/src/vs/obs/text.c"
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
  "${CMAKE_SOURCE_DIR}/src/vs/obs/vendor-api.c"
  "${CMAKE_SOURCE_DIR}/src/vs/tubester/speed.c"
  "${CMAKE_SOURCE_DIR}/src/vs/tubester/command.c"
  ${_analyzer_srcs}
)
target_link_libraries(vs-test-vendor-api PRIVATE plugin-support)
visual_sidechain_add_test(
  test-modulator-properties
  test-modulator-properties.c
  "${CMAKE_SOURCE_DIR}/src/vs/obs/modulator-filter.c"
  "${CMAKE_SOURCE_DIR}/src/vs/obs/audio-tap.c"
  "${CMAKE_SOURCE_DIR}/src/vs/obs/text.c"
  "${_core}/modulation.c"
  "${_core}/baseline.c"
  "${_core}/text-format.c"
  ${_analyzer_srcs}
)
target_link_libraries(vs-test-modulator-properties PRIVATE plugin-support)

# Tubester's core (docs/tubester/REQUIREMENTS.md): pure C, no OBS running
set(_tubester "${CMAKE_SOURCE_DIR}/src/vs/tubester")
visual_sidechain_add_test(test-tubester-core test-tubester-core.c "${_tubester}/tape-url.c" "${_tubester}/speed.c")
visual_sidechain_add_test(
  test-tubester-link
  test-tubester-link.c
  "${_tubester}/deck-link.c"
  "${_tubester}/command.c"
  "${_tubester}/speed.c"
)
# The report listener (R-LNK-2): request parsing and the token source alone, then the listener over a real loopback
# socket. Winsock and BCrypt on Windows, POSIX sockets and arc4random_buf on macOS (R-PLAT-9).
visual_sidechain_add_test(test-tubester-http test-tubester-http.c "${_tubester}/http-parse.c" "${_tubester}/random.c")
if(OS_WINDOWS)
  target_link_libraries(vs-test-tubester-http PRIVATE bcrypt)
endif()
visual_sidechain_add_test(
  test-tubester-listener
  test-tubester-listener.c
  "${_tubester}/listener.c"
  "${_tubester}/http-parse.c"
  "${_tubester}/deck-link.c"
)
if(OS_WINDOWS)
  target_link_libraries(vs-test-tubester-listener PRIVATE ws2_32)
endif()
# The Tape Deck filter itself on a headless libobs (no OBS executable): attaching after Paste Filters Undo / Redo, and a
# deck_command in the gap between a deck's release and its destroy.
visual_sidechain_add_test(
  test-tubester-deck
  test-tubester-deck.c
  "${_tubester}/deck-filter.c"
  "${_tubester}/command.c"
  "${_tubester}/deck-link.c"
  "${_tubester}/http-parse.c"
  "${_tubester}/listener.c"
  "${_tubester}/random.c"
  "${_tubester}/speed.c"
  "${_tubester}/tape-url.c"
  "${_core}/fader-bridge.c"
  "${_core}/tempo.c"
  "${_core}/text-format.c"
  "${CMAKE_SOURCE_DIR}/src/vs/obs/text.c"
)
target_link_libraries(vs-test-tubester-deck PRIVATE plugin-support)
if(OS_WINDOWS)
  target_link_libraries(vs-test-tubester-deck PRIVATE ws2_32 bcrypt)
endif()

# Every built-in shader built by OBS's own renderers (R-PLAT-9): on macOS, libobs-opengl (HLSL to GLSL) and
# libobs-metal (HLSL to Metal), built from the same OBS sources by cmake/common/buildspec_common.cmake. The test exits
# 77 (reported as skipped) when no renderer can create a device on the machine.
if(OS_MACOS)
  file(GLOB _obs_build_dirs LIST_DIRECTORIES true "${CMAKE_SOURCE_DIR}/.deps/obs-studio-*/build_universal")
  set(_gpu_modules "")
  foreach(_module IN ITEMS libobs-opengl libobs-metal)
    set(_found "")
    foreach(_dir IN LISTS _obs_build_dirs)
      file(GLOB_RECURSE _candidates LIST_DIRECTORIES false "${_dir}/${_module}/*${_module}.dylib")
      list(FILTER _candidates EXCLUDE REGEX "[.]dSYM/")
      list(FILTER _candidates INCLUDE REGEX "/Release/")
      if(_candidates)
        list(GET _candidates 0 _found)
        break()
      endif()
    endforeach()
    if(_found)
      list(APPEND _gpu_modules "${_found}")
    elseif(DEFINED ENV{CI})
      message(FATAL_ERROR "Visual Sidechain tests: ${_module}.dylib not found; test-shader-gpu needs it on CI")
    else()
      message(WARNING "Visual Sidechain tests: ${_module}.dylib not found; test-shader-gpu will not use it")
    endif()
  endforeach()
  message(STATUS "Visual Sidechain tests: renderer modules: ${_gpu_modules}")
  if(_gpu_modules)
    list(JOIN _gpu_modules "|" _gpu_modules_define)
    visual_sidechain_add_test(test-shader-gpu test-shader-gpu.c "${_core}/shader-parse.c")
    target_compile_definitions(vs-test-shader-gpu PRIVATE "VS_GPU_MODULES=\"${_gpu_modules_define}\"")
    set_tests_properties(vs-test-shader-gpu PROPERTIES SKIP_RETURN_CODE 77)
  endif()
endif()

# Every locale key used in src/ is in en-US.ini (R-TST-4); the second test proves the check fails on a missing key
add_test(
  NAME vs-check-locale-keys
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/check-locale-keys.cmake"
)
add_test(
  NAME vs-check-locale-keys-fails-on-missing-key
  COMMAND
    "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
    "-DEXTRA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/locale-missing-key.c" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/check-locale-keys.cmake"
)
set_tests_properties(
  vs-check-locale-keys-fails-on-missing-key
  PROPERTIES PASS_REGULAR_EXPRESSION "LocaleKeyThatIsNotInTheLocaleFile \\(.*locale-missing-key\\.c\\)"
)
add_test(
  NAME vs-check-locale-keys-fails-on-changed-english
  COMMAND
    "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
    "-DEXTRA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/locale-format-mismatch.c" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/check-locale-keys.cmake"
)
set_tests_properties(
  vs-check-locale-keys-fails-on-changed-english
  PROPERTIES PASS_REGULAR_EXPRESSION "Shader\.Error\.FileNotFound: the English in .*locale-format-mismatch\.c"
)

# Every translation matches en-US.ini (R-I18N-4): the real files with the twelve required languages, a file full of
# awkward characters must pass, and each kind of fault must be caught (check-locale-files-bad.cmake)
add_test(
  NAME vs-check-locale-files
  COMMAND
    "${CMAKE_COMMAND}" "-DLOCALE_DIR=${CMAKE_SOURCE_DIR}/data/locale" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
    # the twelve languages of R-I18N-4: a missing file fails
    "-DREQUIRED_LOCALES=zh-CN,es-ES,pt-BR,ru-RU,ja-JP,de-DE,fr-FR,ko-KR,zh-TW,tr-TR,nl-NL,it-IT" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/check-locale-files.cmake"
)
add_test(
  NAME vs-check-locale-files-passes-a-good-file
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
    NAME vs-check-locale-files-catches-${_name}
    COMMAND
      "${CMAKE_COMMAND}" "-DFIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/locale-bad"
      "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/locale-bad-${_name}" -P
      "${CMAKE_CURRENT_SOURCE_DIR}/check-locale-files-bad.cmake"
  )
  set_tests_properties(vs-check-locale-files-catches-${_name} PROPERTIES PASS_REGULAR_EXPRESSION "${_expected}")
endforeach()

# The built-in shaders' parameter texts are in en-US.ini word for word (R-I18N-1), so the translations follow them; the
# second test proves a missing, a changed and an extra text are each caught
add_test(
  NAME vs-check-shader-locale
  COMMAND
    "${CMAKE_COMMAND}" "-DSHADER_DIR=${CMAKE_SOURCE_DIR}/data/shaders"
    "-DLOCALE_FILE=${CMAKE_SOURCE_DIR}/data/locale/en-US.ini" -P "${CMAKE_CURRENT_SOURCE_DIR}/check-shader-locale.cmake"
)
add_test(
  NAME vs-check-shader-locale-catches-faults
  COMMAND
    "${CMAKE_COMMAND}" "-DSHADER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/shader-locale-bad"
    "-DLOCALE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/shader-locale-bad/en-US.ini" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/check-shader-locale.cmake"
)
set_tests_properties(
  vs-check-shader-locale-catches-faults
  PROPERTIES PASS_REGULAR_EXPRESSION "shader locale: 1 missing, 1 different, 1 extra"
)

# Built-in shaders may only use raw transient uniforms for allow-listed spatial geometry, never colour/luminance (P0-1)
add_test(
  NAME vs-check-shader-flash-safety
  COMMAND
    "${CMAKE_COMMAND}" "-DSHADER_DIR=${CMAKE_SOURCE_DIR}/data/shaders" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/check-shader-flash-safety.cmake"
)
add_test(
  NAME vs-check-shader-flash-safety-catches-faults
  COMMAND
    "${CMAKE_COMMAND}" "-DSHADER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/shader-flash-bad" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/check-shader-flash-safety.cmake"
)
set_tests_properties(
  vs-check-shader-flash-safety-catches-faults
  PROPERTIES PASS_REGULAR_EXPRESSION "shader flash safety: .*ungated-flash\\.shader.*unguarded raw transient"
)

# Built-in shaders use no HLSL-only form that the Mac's GLSL and Metal compilers refuse (R-PLAT-9), checked on every
# platform; the second test proves a cast and an attribute are each caught
add_test(
  NAME vs-check-shader-portability
  COMMAND
    "${CMAKE_COMMAND}" "-DSHADER_DIR=${CMAKE_SOURCE_DIR}/data/shaders" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/check-shader-portability.cmake"
)
add_test(
  NAME vs-check-shader-portability-catches-faults
  COMMAND
    "${CMAKE_COMMAND}" "-DSHADER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/shader-portability-bad" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/check-shader-portability.cmake"
)
set_tests_properties(
  vs-check-shader-portability-catches-faults
  PROPERTIES
    PASS_REGULAR_EXPRESSION
      "hlsl-only\\.shader: C-style cast '\\(float\\)i'.*hlsl-only\\.shader: HLSL attribute '\\[unroll\\]'"
)

# Suite naming pass invariants across all locale files (P1-7)
add_test(
  NAME vs-check-product-naming
  COMMAND
    "${CMAKE_COMMAND}" "-DLOCALE_DIR=${CMAKE_SOURCE_DIR}/data/locale" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/check-product-naming.cmake"
)
add_test(
  NAME vs-check-product-naming-catches-faults
  COMMAND
    "${CMAKE_COMMAND}" "-DLOCALE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/product-naming-bad" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/check-product-naming.cmake"
)
set_tests_properties(
  vs-check-product-naming-catches-faults
  PROPERTIES
    PASS_REGULAR_EXPRESSION
      "product naming check failed:.*contains legacy product name 'Stream Deck Fadester'.*Hotkey\\.Restore must say 'Restore visuals'"
)
