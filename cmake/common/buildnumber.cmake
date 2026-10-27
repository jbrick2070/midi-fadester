# CMake build number module
# Copyright (C) 2026 Jeffrey A. Brick
# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

set(
  _BUILD_NUMBER_CACHE
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/.CMakeBuildNumber"
  CACHE INTERNAL
  "OBS build number cache file"
)

if(NOT DEFINED PLUGIN_BUILD_NUMBER)
  if(EXISTS "${_BUILD_NUMBER_CACHE}")
    file(READ "${_BUILD_NUMBER_CACHE}" PLUGIN_BUILD_NUMBER)
    math(EXPR PLUGIN_BUILD_NUMBER "${PLUGIN_BUILD_NUMBER}+1")
  else()
    if("$ENV{CI}")
      if("$ENV{GITHUB_RUN_ID}")
        set(PLUGIN_BUILD_NUMBER "$ENV{GITHUB_RUN_ID}")
      elseif("$ENV{GITLAB_RUN_ID}")
        set(PLUGIN_BUILD_NUMBER "$ENV{GITLAB_RUN_ID}")
      else()
        set(PLUGIN_BUILD_NUMBER "1")
      endif()
    else()
      set(PLUGIN_BUILD_NUMBER "1")
    endif()
  endif()
  file(WRITE "${_BUILD_NUMBER_CACHE}" "${PLUGIN_BUILD_NUMBER}")
endif()
