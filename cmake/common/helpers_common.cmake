# CMake common helper functions module
# Copyright (C) 2026 Jeffrey A. Brick
# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

function(check_uuid uuid_string return_value)
  set(valid_uuid TRUE)
  # gersemi: off
  set(uuid_token_lengths 8 4 4 4 12)
  # gersemi: on
  set(token_num 0)

  string(REPLACE "-" ";" uuid_tokens ${uuid_string})
  list(LENGTH uuid_tokens uuid_num_tokens)

  if(uuid_num_tokens EQUAL 5)
    message(DEBUG "UUID ${uuid_string} is valid with 5 tokens.")
    foreach(uuid_token IN LISTS uuid_tokens)
      list(GET uuid_token_lengths ${token_num} uuid_target_length)
      string(LENGTH "${uuid_token}" uuid_actual_length)
      if(uuid_actual_length EQUAL uuid_target_length)
        string(REGEX MATCH "[0-9a-fA-F]+" uuid_hex_match ${uuid_token})
        if(NOT uuid_hex_match STREQUAL uuid_token)
          set(valid_uuid FALSE)
          break()
        endif()
      else()
        set(valid_uuid FALSE)
        break()
      endif()
      math(EXPR token_num "${token_num}+1")
    endforeach()
  else()
    set(valid_uuid FALSE)
  endif()
  message(DEBUG "UUID ${uuid_string} valid: ${valid_uuid}")
  set(${return_value} ${valid_uuid} PARENT_SCOPE)
endfunction()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin-support.c.in")
  set(_mfd_hdr "${CMAKE_BINARY_DIR}/generated/mfd-build-info.h")
  set(_mfd_gen "${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/generate_build_info.cmake")
  set(_mfd_src "${CMAKE_CURRENT_SOURCE_DIR}")

  execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${_mfd_src}" "-DOUTPUT_HEADER=${_mfd_hdr}" -P "${_mfd_gen}"
  )

  add_custom_target(
    mfd-build-info
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${_mfd_src}" "-DOUTPUT_HEADER=${_mfd_hdr}" -P "${_mfd_gen}"
    BYPRODUCTS "${_mfd_hdr}"
    COMMENT "Updating build-time git commit and timestamp"
    VERBATIM
  )

  configure_file(src/plugin-support.c.in plugin-support.c @ONLY)
  add_library(plugin-support STATIC)
  add_dependencies(plugin-support mfd-build-info)
  target_sources(plugin-support PRIVATE plugin-support.c "${_mfd_hdr}" PUBLIC src/plugin-support.h)
  target_include_directories(
    plugin-support
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PRIVATE "${CMAKE_BINARY_DIR}/generated"
  )
  if(OS_LINUX OR OS_FREEBSD OR OS_OPENBSD)
    set_property(TARGET plugin-support PROPERTY POSITION_INDEPENDENT_CODE ON)
  endif()
endif()
