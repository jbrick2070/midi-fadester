# CMake macOS compiler configuration module

include_guard(GLOBAL)

option(ENABLE_COMPILER_TRACE "Enable clang time-trace" OFF)
mark_as_advanced(ENABLE_COMPILER_TRACE)

if(NOT XCODE)
  message(FATAL_ERROR "Building OBS Studio on macOS requires Xcode generator.")
endif()

include(ccache)
include(compiler_common)

# Xcode 26+ exports default deployment targets for every Apple platform into
# Run Script build phases, so universal AUTOMOC predefines invocations need an
# explicit macOS deployment target flag to disambiguate clang++.
if(CMAKE_CXX_COMPILER_PREDEFINES_COMMAND AND CMAKE_OSX_DEPLOYMENT_TARGET)
  set(_mfd_predef_target_flag "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
  if(NOT _mfd_predef_target_flag IN_LIST CMAKE_CXX_COMPILER_PREDEFINES_COMMAND)
    list(APPEND CMAKE_CXX_COMPILER_PREDEFINES_COMMAND "${_mfd_predef_target_flag}")
  endif()
  unset(_mfd_predef_target_flag)
endif()

add_compile_options("$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:-fopenmp-simd>")

# Ensure recent enough Xcode and platform SDK
set(_obs_macos_minimum_sdk 15.0) # Keep in sync with Xcode
set(_obs_macos_minimum_xcode 16.0) # Keep in sync with SDK
message(DEBUG "macOS SDK Path: ${CMAKE_OSX_SYSROOT}")
execute_process(
  COMMAND xcrun --sdk macosx --show-sdk-version
  OUTPUT_VARIABLE _obs_macos_current_sdk
  OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY
)
message(DEBUG "macOS SDK version: ${_obs_macos_current_sdk}")
if(_obs_macos_current_sdk VERSION_LESS _obs_macos_minimum_sdk)
  message(
    FATAL_ERROR
    "Your macOS SDK version (${_obs_macos_current_sdk}) is too low. "
    "The macOS ${_obs_macos_minimum_sdk} SDK (Xcode ${_obs_macos_minimum_xcode}) is required to build OBS."
  )
endif()
unset(_obs_macos_current_sdk)
unset(_obs_macos_minimum_sdk)
unset(_obs_macos_minimum_xcode)

# Enable dSYM generator for release builds
string(APPEND CMAKE_C_FLAGS_RELEASE " -g")
string(APPEND CMAKE_CXX_FLAGS_RELEASE " -g")
string(APPEND CMAKE_OBJC_FLAGS_RELEASE " -g")
string(APPEND CMAKE_OBJCXX_FLAGS_RELEASE " -g")

# Default ObjC compiler options used by Xcode:
#
# * -Wno-implicit-atomic-properties
# * -Wno-objc-interface-ivars
# * -Warc-repeated-use-of-weak
# * -Wno-arc-maybe-repeated-use-of-weak
# * -Wimplicit-retain-self
# * -Wduplicate-method-match
# * -Wshadow
# * -Wfloat-conversion
# * -Wobjc-literal-conversion
# * -Wno-selector
# * -Wno-strict-selector-match
# * -Wundeclared-selector
# * -Wdeprecated-implementations
# * -Wprotocol
# * -Werror=block-capture-autoreleasing
# * -Wrange-loop-analysis

# Default ObjC++ compiler options used by Xcode:
#
# * -Wno-non-virtual-dtor

add_compile_definitions(
  $<$<NOT:$<COMPILE_LANGUAGE:Swift>>:$<$<CONFIG:DEBUG>:DEBUG>>
  $<$<NOT:$<COMPILE_LANGUAGE:Swift>>:$<$<CONFIG:DEBUG>:_DEBUG>>
  $<$<NOT:$<COMPILE_LANGUAGE:Swift>>:SIMDE_ENABLE_OPENMP>
)

if(ENABLE_COMPILER_TRACE)
  add_compile_options(
    $<$<NOT:$<COMPILE_LANGUAGE:Swift>>:-ftime-trace>
    "$<$<COMPILE_LANGUAGE:Swift>:SHELL:-Xfrontend -debug-time-expression-type-checking>"
    "$<$<COMPILE_LANGUAGE:Swift>:SHELL:-Xfrontend -debug-time-function-bodies>"
  )
  add_link_options(LINKER:-print_statistics)
endif()
