# Midi Fadester CTest suite
# Copyright (C) 2026 Jeffrey A. Brick
# SPDX-License-Identifier: GPL-2.0-or-later

if(OS_MACOS)
  # Test executables live outside OBS.app and need the development frameworks and dylibs.
  set(CMAKE_SKIP_RPATH FALSE)
  set(CMAKE_XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "")
  set(CMAKE_BUILD_RPATH "${CMAKE_SOURCE_DIR}/.deps/Frameworks;${CMAKE_SOURCE_DIR}/.deps/lib")
  foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
    list(APPEND CMAKE_BUILD_RPATH "${_prefix}/lib")
  endforeach()
endif()

set(_test_dll_dirs "")
foreach(_config IN ITEMS RELEASE RELWITHDEBINFO DEBUG NOCONFIG)
  get_target_property(_libobs_location OBS::libobs IMPORTED_LOCATION_${_config})
  if(_libobs_location)
    cmake_path(GET _libobs_location PARENT_PATH _libobs_dir)
    list(APPEND _test_dll_dirs "${_libobs_dir}")
    break()
  endif()
endforeach()

set(_qt_plugins_dir "")
foreach(_config IN ITEMS RELEASE RELWITHDEBINFO DEBUG NOCONFIG)
  get_target_property(_qtcore_location Qt6::Core IMPORTED_LOCATION_${_config})
  if(_qtcore_location)
    cmake_path(GET _qtcore_location PARENT_PATH _qt_bin_dir)
    list(APPEND _test_dll_dirs "${_qt_bin_dir}")
    cmake_path(GET _qt_bin_dir PARENT_PATH _qt_root_dir)
    if(EXISTS "${_qt_root_dir}/plugins/platforms/qwindows.dll")
      set(_qt_plugins_dir "${_qt_root_dir}/plugins")
    endif()
    break()
  endif()
endforeach()

foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
  if(EXISTS "${_prefix}/share/obs-deps/VERSION" AND EXISTS "${_prefix}/bin")
    list(APPEND _test_dll_dirs "${_prefix}/bin")
  endif()
  if(NOT _qt_plugins_dir AND EXISTS "${_prefix}/plugins/platforms/qwindows.dll")
    set(_qt_plugins_dir "${_prefix}/plugins")
  endif()
endforeach()
list(REMOVE_DUPLICATES _test_dll_dirs)

set(_test_environment "")
foreach(_dir IN LISTS _test_dll_dirs)
  cmake_path(NATIVE_PATH _dir _native_dir)
  list(APPEND _test_environment "PATH=path_list_prepend:${_native_dir}")
endforeach()

set(_ui_test_environment ${_test_environment})
if(OS_MACOS)
  foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${_prefix}/plugins/platforms/libqcocoa.dylib")
      set(_qt_plugins_dir "${_prefix}/plugins")
      break()
    endif()
  endforeach()
endif()
if(_qt_plugins_dir)
  cmake_path(NATIVE_PATH _qt_plugins_dir _native_qt_plugins)
  list(APPEND _ui_test_environment "QT_PLUGIN_PATH=set:${_native_qt_plugins}")
  message(STATUS "midi-fadester tests: Qt6 plugins dir: ${_qt_plugins_dir}")
endif()
message(STATUS "midi-fadester tests: DLL search dirs: ${_test_dll_dirs}")

function(midi_fadester_add_test name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(${name} PRIVATE OBS::libobs)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT_MODIFICATION "${_test_environment}")
endfunction()

set(_core "${CMAKE_SOURCE_DIR}/src/core")
midi_fadester_add_test(test-mfd-text test-mfd-text.c "${_core}/mfd-text.c" "${_core}/slot-messages.c")
midi_fadester_add_test(test-midi-queue test-midi-queue.c "${_core}/midi-queue.c")
midi_fadester_add_test(test-midi-parser test-midi-parser.c "${_core}/midi-parser.c" "${_core}/mfd-text.c")
midi_fadester_add_test(test-smoother test-smoother.c "${_core}/smoother.c")
midi_fadester_add_test(test-takeover test-takeover.c "${_core}/takeover.c")
midi_fadester_add_test(test-mixer-math test-mixer-math.c "${_core}/mixer-math.c")
midi_fadester_add_test(test-apc40-profile test-apc40-profile.c "${_core}/apc40-profile.c" "${_core}/mfd-text.c")
midi_fadester_add_test(test-tbar-logic test-tbar-logic.c "${_core}/tbar-logic.c")
midi_fadester_add_test(test-filter-mod test-filter-mod.c "${_core}/filter-math.c" "${_core}/mfd-text.c")
midi_fadester_add_test(
  test-ui-sentences
  test-ui-sentences.c
  "${_core}/ui-sentences.c"
  "${_core}/slot-messages.c"
  "${_core}/mfd-text.c"
  "${_core}/apc40-profile.c"
  "${_core}/takeover.c"
  "${_core}/filter-math.c"
  "${_core}/midi-parser.c"
)
target_compile_definitions(
  test-ui-sentences
  PRIVATE "MFD_TEST_LOCALE_FILE=\"${CMAKE_SOURCE_DIR}/data/locale/en-US.ini\""
)

add_executable(
  test-vendor-api
  test-vendor-api.c
  "${CMAKE_SOURCE_DIR}/src/obs/mfd-vendor-api.c"
  "${CMAKE_SOURCE_DIR}/src/obs/mfd-engine.c"
  "${CMAKE_SOURCE_DIR}/src/obs/mfd-layer-filter.c"
  "${_midi_backend}"
  "${_core}/midi-queue.c"
  "${_core}/midi-parser.c"
  "${_core}/smoother.c"
  "${_core}/takeover.c"
  "${_core}/mixer-math.c"
  "${_core}/apc40-profile.c"
  "${_core}/tbar-logic.c"
  "${_core}/filter-math.c"
  "${_core}/ui-sentences.c"
  "${_core}/slot-messages.c"
  "${_core}/mfd-text.c"
)
target_include_directories(test-vendor-api PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(test-vendor-api PRIVATE "MFD_TEST_LOCALE_FILE=\"${CMAKE_SOURCE_DIR}/data/locale/en-US.ini\"")
target_link_libraries(test-vendor-api PRIVATE OBS::libobs OBS::obs-frontend-api plugin-support ${_midi_libraries})
add_test(NAME test-vendor-api COMMAND test-vendor-api)
set_tests_properties(test-vendor-api PROPERTIES ENVIRONMENT_MODIFICATION "${_test_environment}")

add_executable(
  test-ui-dialog
  test-ui-dialog.cpp
  "${CMAKE_SOURCE_DIR}/src/ui/mfd-text-qt.hpp"
  "${CMAKE_SOURCE_DIR}/src/ui/mfd-text-qt.cpp"
  "${CMAKE_SOURCE_DIR}/src/ui/mfd-svg-canvas.hpp"
  "${CMAKE_SOURCE_DIR}/src/ui/mfd-controller-svg.hpp"
  "${CMAKE_SOURCE_DIR}/src/ui/mfd-controller-svg.cpp"
  "${CMAKE_SOURCE_DIR}/src/ui/mfd-setup-sheet.hpp"
  "${CMAKE_SOURCE_DIR}/src/ui/mfd-setup-sheet.cpp"
  "${CMAKE_SOURCE_DIR}/src/ui/mfd-settings-dialog.hpp"
  "${CMAKE_SOURCE_DIR}/src/ui/mfd-settings-dialog.cpp"
  "${CMAKE_SOURCE_DIR}/src/obs/mfd-vendor-api.c"
  "${CMAKE_SOURCE_DIR}/src/obs/mfd-engine.c"
  "${CMAKE_SOURCE_DIR}/src/obs/mfd-layer-filter.c"
  "${_midi_backend}"
  "${_core}/midi-queue.c"
  "${_core}/midi-parser.c"
  "${_core}/smoother.c"
  "${_core}/takeover.c"
  "${_core}/mixer-math.c"
  "${_core}/apc40-profile.c"
  "${_core}/tbar-logic.c"
  "${_core}/filter-math.c"
  "${_core}/ui-sentences.c"
  "${_core}/slot-messages.c"
  "${_core}/mfd-text.c"
)
set_target_properties(test-ui-dialog PROPERTIES AUTOMOC ON)
target_compile_definitions(
  test-ui-dialog
  PRIVATE QT_NO_KEYWORDS "MFD_TEST_LOCALE_FILE=\"${CMAKE_SOURCE_DIR}/data/locale/en-US.ini\""
)
target_include_directories(test-ui-dialog PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(
  test-ui-dialog
  PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg plugin-support ${_midi_libraries}
)
add_test(NAME test-ui-dialog COMMAND test-ui-dialog)
set_tests_properties(test-ui-dialog PROPERTIES ENVIRONMENT_MODIFICATION "${_ui_test_environment}")

add_executable(midi-send midi-send.c)
if(OS_WINDOWS)
  target_link_libraries(midi-send PRIVATE winmm)
endif()
add_test(NAME test-midi-send COMMAND midi-send --self-test)
set_tests_properties(test-midi-send PROPERTIES ENVIRONMENT_MODIFICATION "${_test_environment}")

set(
  _production_guard_files
  "${CMAKE_SOURCE_DIR}/src/plugin-main.c"
  "${_core}/midi-types.h"
  "${_core}/midi-queue.h"
  "${_core}/midi-queue.c"
  "${_core}/midi-parser.h"
  "${_core}/midi-parser.c"
  "${_core}/smoother.h"
  "${_core}/smoother.c"
  "${_core}/takeover.h"
  "${_core}/takeover.c"
  "${_core}/mixer-math.h"
  "${_core}/mixer-math.c"
  "${_core}/apc40-profile.h"
  "${_core}/apc40-profile.c"
  "${_core}/tbar-logic.h"
  "${_core}/tbar-logic.c"
  "${_core}/filter-math.h"
  "${_core}/filter-math.c"
  "${_core}/ui-sentences.h"
  "${_core}/slot-messages.h"
  "${_core}/mfd-text.h"
  "${_core}/ui-sentences.c"
  "${_core}/slot-messages.c"
  "${_core}/mfd-text.c"
  "${CMAKE_SOURCE_DIR}/src/midi/winmm-device.h"
  "${_midi_backend}"
  "${CMAKE_SOURCE_DIR}/src/obs/mfd-layer-filter.h"
  "${CMAKE_SOURCE_DIR}/src/obs/mfd-layer-filter.c"
  "${CMAKE_SOURCE_DIR}/src/obs/mfd-engine.h"
  "${CMAKE_SOURCE_DIR}/src/obs/mfd-engine.c"
  "${CMAKE_SOURCE_DIR}/src/obs/mfd-vendor-api.h"
  "${CMAKE_SOURCE_DIR}/src/obs/mfd-vendor-api.c"
  "${CMAKE_SOURCE_DIR}/src/ui/mfd-text-qt.hpp"
  "${CMAKE_SOURCE_DIR}/src/ui/mfd-text-qt.cpp"
  "${CMAKE_SOURCE_DIR}/src/ui/mfd-svg-canvas.hpp"
  "${CMAKE_SOURCE_DIR}/src/ui/mfd-controller-svg.hpp"
  "${CMAKE_SOURCE_DIR}/src/ui/mfd-controller-svg.cpp"
  "${CMAKE_SOURCE_DIR}/src/ui/mfd-setup-sheet.hpp"
  "${CMAKE_SOURCE_DIR}/src/ui/mfd-setup-sheet.cpp"
  "${CMAKE_SOURCE_DIR}/src/ui/mfd-settings-dialog.hpp"
  "${CMAKE_SOURCE_DIR}/src/ui/mfd-settings-dialog.cpp"
)

add_executable(test-shader-syntax test-shader-syntax.c)
add_test(
  NAME test-shader-syntax
  COMMAND test-shader-syntax "${CMAKE_SOURCE_DIR}/data/effects/mfd_layer_mixer.effect" ${_production_guard_files}
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
# the key check reads calls wrapped over lines; it fails on English that differs from en-US.ini and on a key built at
# run time
foreach(
  _case
  IN
  ITEMS
    "reads-multi-line-calls|locale-multi-line.c|locale keys: every key used"
    "fails-on-changed-english|locale-changed-english.c|English in the code differs.*PluginDisplayName.*locale-changed-english\\.c"
    "fails-on-changed-wrapped-english|locale-multi-line-changed.c|English in the code differs.*PluginDisplayName.*locale-multi-line-changed\\.c"
    "fails-on-runtime-key|locale-runtime-key.c|built at run time.*key_from_somewhere"
)
  string(REPLACE "|" ";" _parts "${_case}")
  list(GET _parts 0 _name)
  list(GET _parts 1 _fixture)
  list(GET _parts 2 _expected)
  add_test(
    NAME check-locale-keys-${_name}
    COMMAND
      "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
      "-DEXTRA_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/${_fixture}" -P
      "${CMAKE_CURRENT_SOURCE_DIR}/check-locale-keys.cmake"
  )
  set_tests_properties(check-locale-keys-${_name} PROPERTIES PASS_REGULAR_EXPRESSION "${_expected}")
endforeach()

# every translation matches en-US.ini (R-I18N-5), and the eleven languages of R-I18N-4 are all there. A good file with
# awkward characters must pass, and each kind of fault must be caught (check-locale-files-bad.cmake)
add_test(
  NAME check-locale-files
  COMMAND
    "${CMAKE_COMMAND}" "-DLOCALE_DIR=${CMAKE_SOURCE_DIR}/data/locale"
    "-DREQUIRED_LOCALES=zh-CN,es-ES,pt-BR,ru-RU,ja-JP,de-DE,fr-FR,ko-KR,zh-TW,tr-TR,nl-NL" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/check-locale-files.cmake"
)
add_test(
  NAME check-locale-files-passes-a-good-file
  COMMAND
    "${CMAKE_COMMAND}" "-DLOCALE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/locale-good" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/check-locale-files.cmake"
)
set_tests_properties(
  check-locale-files-passes-a-good-file
  PROPERTIES PASS_REGULAR_EXPRESSION "1 translations checked" FAIL_REGULAR_EXPRESSION "CMake Error"
)
foreach(
  _case
  IN
  ITEMS
    "missing-key|de-DE.ini: missing key Gone"
    "extra-key|de-DE.ini: extra key Extra"
    "repeated-key|de-DE.ini: key Extra appears more than once"
    "placeholder|de-DE.ini: Greeting has placeholders"
    "stray-quote|de-DE.ini: Plain has a quote that is not escaped"
    "printf|de-DE.ini: Printf has a printf conversion"
    "mnemonic|de-DE.ini: Mnemonic has 0 Qt mnemonic"
    "bom|fr-FR.ini: starts with a BOM"
    "invalid-utf8|fr-FR.ini: not valid UTF-8"
    "file-name|german.ini: not named like de-DE.ini"
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
