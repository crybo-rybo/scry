# Include after ScryCompilerChecks.cmake and add_library(scry ...).
add_library(scry_project_options INTERFACE)
# ScryCompilerChecks admits only GCC and Clang-family compilers, and every
# project that links this target compiles C++ alone.
target_compile_options(
  scry_project_options
  INTERFACE -Wall -Wextra -Wconversion -Wshadow
)

# A tooling compiler older than the host's libstdc++ cannot parse its headers —
# clang 18 against Ubuntu 24.04's <expected>, for one — so the tooling build can
# select libc++ instead. No consumer build is affected: SCRY_CLANG_TOOLING is
# not one. Every tooling target, scry included, links scry_project_options, so
# the flags ride on it alone.
if(SCRY_CLANG_TOOLING_LIBCXX)
  target_compile_options(scry_project_options INTERFACE -stdlib=libc++)
  target_link_options(scry_project_options INTERFACE -stdlib=libc++)
endif()

if(SCRY_CLANG_TOOLING AND NOT SCRY_TOOLING_HAS_STOP_TOKEN)
  target_compile_options(scry_project_options INTERFACE -fexperimental-library)
  target_link_options(scry_project_options INTERFACE -fexperimental-library)
endif()

if(SCRY_WARNINGS_AS_ERRORS)
  target_compile_options(scry_project_options INTERFACE -Werror)
endif()

function(scry_enable_sanitizer FLAG)
  target_compile_options(
    scry_project_options
    INTERFACE "${FLAG}" -fno-omit-frame-pointer
  )
  target_link_options(
    scry_project_options
    INTERFACE "${FLAG}" -fno-omit-frame-pointer
  )
endfunction()

if(SCRY_SANITIZER STREQUAL "address-undefined")
  scry_enable_sanitizer("-fsanitize=address,undefined")
  target_compile_options(
    scry_project_options
    INTERFACE -fno-sanitize-recover=undefined
  )
elseif(SCRY_SANITIZER STREQUAL "thread")
  scry_enable_sanitizer("-fsanitize=thread")
elseif(NOT SCRY_SANITIZER STREQUAL "none")
  message(FATAL_ERROR "Unsupported SCRY_SANITIZER: ${SCRY_SANITIZER}")
endif()

# libFuzzer can only steer through code carrying SanitizerCoverage, and targets
# like scry_conversation_fuzz link the whole library, so without this every
# mutation past the fuzz entry point would be blind. The "-no-link" spelling adds
# the instrumentation without libFuzzer's main, which ordinary test executables
# must not link; the fuzz targets add plain -fsanitize=fuzzer themselves.
if(SCRY_BUILD_FUZZERS)
  target_compile_options(
    scry_project_options
    INTERFACE -fsanitize=fuzzer-no-link
  )
endif()

if(SCRY_ENABLE_CLANG_TIDY)
  find_program(SCRY_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
  set_property(
    TARGET scry
    PROPERTY
      CXX_CLANG_TIDY
        "${SCRY_CLANG_TIDY_EXECUTABLE};--warnings-as-errors=*"
  )
endif()

if(NOT SCRY_CLANG_TOOLING)
  add_custom_target(
    scry_header_audit ALL
    COMMAND
      "${CMAKE_COMMAND}"
      "-DSCRY_PUBLIC_INCLUDE_DIR=${PROJECT_SOURCE_DIR}/include"
      "-DSCRY_GENERATED_INCLUDE_DIR=${SCRY_GENERATED_INCLUDE_DIR}"
      -P "${PROJECT_SOURCE_DIR}/cmake/CheckPublicHeaders.cmake"
    COMMENT "Auditing the public-header boundary"
    VERBATIM
  )
endif()
