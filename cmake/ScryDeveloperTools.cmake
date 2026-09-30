# Include after ScryCompilerChecks.cmake and add_library(scry_kernel ...).
add_library(scry_project_options INTERFACE)
# ScryCompilerChecks admits only GCC and Clang-family compilers, and every
# project that links this target compiles C++ alone.
target_compile_options(
  scry_project_options
  INTERFACE -Wall -Wextra -Wconversion -Wshadow
)

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

# libFuzzer can only steer through code carrying SanitizerCoverage, and the fuzz
# targets link the scry_kernel objects rather than compiling the code under test
# themselves, so without this every mutation past the fuzz entry point would be
# blind. The "-no-link" spelling adds the instrumentation without libFuzzer's
# main, which the kernel objects must not carry; the fuzz targets add plain
# -fsanitize=fuzzer themselves.
if(SCRY_BUILD_FUZZERS)
  target_compile_options(
    scry_project_options
    INTERFACE -fsanitize=fuzzer-no-link
  )
endif()

# clang-tidy analyzes the kernel only: the rest of src/ is C++26 and may use
# reflection, which clang-tidy cannot parse.
if(SCRY_ENABLE_CLANG_TIDY)
  find_program(SCRY_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
  set_property(
    TARGET scry_kernel
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
