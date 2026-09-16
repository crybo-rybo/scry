cmake_minimum_required(VERSION 3.28)

project(
  scry
  VERSION 0.4.0
  DESCRIPTION "A main-loop-friendly C++ LLM harness"
  LANGUAGES CXX
)

# The CMake project version above is the only place the release number lives;
# the public <scry/version.hpp> is generated from it so the two cannot drift.
set(
  SCRY_GENERATED_INCLUDE_DIR
  "${PROJECT_BINARY_DIR}/generated/include"
)
set(
  SCRY_GENERATED_VERSION_HEADER
  "${SCRY_GENERATED_INCLUDE_DIR}/scry/version.hpp"
)
configure_file(
  cmake/version.hpp.in
  "${SCRY_GENERATED_VERSION_HEADER}"
  @ONLY
)

# Scry uses no C++ modules. From CMake 3.28 the default is to scan every
# C++20-and-later source for module dependencies, which adds -fmodules-ts to
# each GCC compile; GCC's module mode then fails on libstdc++ headers against
# the macOS SDK. Scanning is off everywhere this project compiles C++.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(CMakePackageConfigHelpers)
include(FetchContent)
include(GNUInstallDirs)

option(SCRY_BUILD_TESTS "Build Scry's tests" ${PROJECT_IS_TOP_LEVEL})
option(SCRY_BUILD_EXAMPLES "Compile Scry's examples" ${PROJECT_IS_TOP_LEVEL})
option(
  SCRY_BUILD_TESTING_SUPPORT
  "Build and install scry::testing, the scripted-transport package component"
  ON
)
option(SCRY_ENABLE_CLANG_TIDY "Run clang-tidy while compiling" OFF)
option(SCRY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ${PROJECT_IS_TOP_LEVEL})
option(
  SCRY_CLANG_TOOLING
  "Build only the portable C++23 implementation with a Clang compiler, for \
clang-tidy and libFuzzer. Not a supported consumer build."
  OFF
)
option(
  SCRY_CLANG_TOOLING_LIBCXX
  "Use libc++ for the Clang tooling build; needed where the host's libstdc++ \
lacks std::expected for the tooling compiler"
  OFF
)
option(SCRY_BUILD_FUZZERS "Build short protocol fuzz targets" OFF)

set(
  SCRY_SANITIZER
  "none"
  CACHE STRING
  "Sanitizer mode: none, address-undefined, or thread"
)
set_property(
  CACHE SCRY_SANITIZER
  PROPERTY STRINGS none address-undefined thread
)

include(ScryCompilerChecks)

find_package(CURL 7.84 REQUIRED)
find_package(Threads REQUIRED)

# FIND_PACKAGE_ARGS lets a packaged Glaze (vcpkg, Conan, Homebrew) satisfy the
# dependency; the pinned fetch is the fallback when find_package(glaze) fails.
FetchContent_Declare(
  glaze
  GIT_REPOSITORY https://github.com/stephenberry/glaze.git
  GIT_TAG dce5e0f7ec572725ec369a0bc59a00eeb2270a9a
  SYSTEM
  EXCLUDE_FROM_ALL
  FIND_PACKAGE_ARGS
)
FetchContent_MakeAvailable(glaze)

# Scry consumes Glaze privately through raw include directories so no
# glaze::glaze link dependency is ever exported. Where those headers live
# depends on which of the two paths above provided them.
if(glaze_SOURCE_DIR)
  set(SCRY_GLAZE_INCLUDE_DIR "${glaze_SOURCE_DIR}/include")
else()
  get_target_property(
    SCRY_GLAZE_INCLUDE_DIR
    glaze::glaze
    INTERFACE_INCLUDE_DIRECTORIES
  )
endif()
if(NOT SCRY_GLAZE_INCLUDE_DIR)
  message(FATAL_ERROR "Could not determine the Glaze include directory")
endif()

add_library(
  scry
  STATIC
    src/core/error.cpp
    src/core/json_codec.cpp
    src/core/json_view.cpp
    src/core/retry.cpp
    src/machine/turn_machine.cpp
    src/protocol/sse.cpp
    src/provider/anthropic_content.cpp
    src/provider/anthropic_request.cpp
    src/provider/anthropic_stream.cpp
    src/provider/openai_content.cpp
    src/provider/openai_request.cpp
    src/provider/openai_stream.cpp
    src/provider/provider.cpp
    src/reflection/json_bridge.cpp
    src/runtime/config.cpp
    src/runtime/conversation.cpp
    src/runtime/conversation_persistence.cpp
    src/runtime/harness.cpp
    src/runtime/pump.cpp
    src/runtime/pump_state.cpp
    src/runtime/queue.cpp
    src/runtime/tool_dispatch.cpp
    src/runtime/tool_registry.cpp
    src/runtime/turn.cpp
    src/runtime/worker.cpp
    src/runtime/worker_messages.cpp
    src/transport/curl_error.cpp
    src/transport/curl_global.cpp
    src/transport/curl_transport.cpp
    src/transport/transport_policy.cpp
)
add_library(scry::scry ALIAS scry)
set_target_properties(
  scry
  PROPERTIES
    EXPORT_NAME scry
    POSITION_INDEPENDENT_CODE ON
)
if(SCRY_CLANG_TOOLING)
  target_compile_features(scry PUBLIC cxx_std_23)
else()
  target_compile_features(scry PUBLIC cxx_std_26)
  target_compile_options(scry PUBLIC -freflection)
  set_target_properties(
    scry
    PROPERTIES
      CXX_EXTENSIONS OFF
      CXX_STANDARD 26
      CXX_STANDARD_REQUIRED ON
  )
endif()
target_include_directories(
  scry
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${SCRY_GENERATED_INCLUDE_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
)
target_include_directories(scry SYSTEM PRIVATE "${SCRY_GLAZE_INCLUDE_DIR}")

include(ScryDeveloperTools)

target_link_libraries(
  scry
  PRIVATE
    CURL::libcurl
    Threads::Threads
    "$<BUILD_INTERFACE:scry_project_options>"
)

set(
  SCRY_CORE_PUBLIC_HEADERS
  include/scry/config.hpp
  include/scry/conversation.hpp
  include/scry/detail/reflection_codec.hpp
  include/scry/detail/reflection_json.hpp
  include/scry/detail/reflection_meta.hpp
  include/scry/detail/reflection_registration.hpp
  include/scry/detail/reflection_schema.hpp
  include/scry/error.hpp
  include/scry/events.hpp
  include/scry/harness.hpp
  include/scry/json.hpp
  include/scry/message.hpp
  include/scry/reflection.hpp
  include/scry/scry.hpp
  include/scry/tool_registry.hpp
  include/scry/turn.hpp
  include/scry/turn_id.hpp
  include/scry/unique_function.hpp
)
target_sources(
  scry
  PUBLIC
    FILE_SET scry_core_headers
    TYPE HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/include"
    FILES ${SCRY_CORE_PUBLIC_HEADERS}
    FILE_SET scry_generated_headers
    TYPE HEADERS
    BASE_DIRS "${SCRY_GENERATED_INCLUDE_DIR}"
    FILES "${SCRY_GENERATED_VERSION_HEADER}"
)

set(
  SCRY_TESTING_PUBLIC_HEADERS
  "${PROJECT_SOURCE_DIR}/include/scry/testing/scripted_transport.hpp"
  "${PROJECT_SOURCE_DIR}/include/scry/testing/streams.hpp"
)
if(SCRY_BUILD_TESTING_SUPPORT)
  add_subdirectory(testing)
endif()

if(SCRY_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# CTest must be enabled here so ctest can discover either set of targets.
if(SCRY_BUILD_TESTS OR SCRY_BUILD_FUZZERS)
  include(CTest)
endif()
if(SCRY_BUILD_TESTS)
  add_subdirectory(tests)
endif()
if(SCRY_BUILD_FUZZERS)
  add_subdirectory(tests/fuzz)
endif()

configure_package_config_file(
  cmake/scryConfig.cmake.in
  "${PROJECT_BINARY_DIR}/scryConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/scry"
)
# Pre-1.0, minor releases may break API; SameMinorVersion keeps a consumer's
# find_package(scry 0.1) from silently accepting an incompatible 0.2 install.
write_basic_package_version_file(
  "${PROJECT_BINARY_DIR}/scryConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMinorVersion
)

install(
  TARGETS scry
  EXPORT scryTargets
  FILE_SET scry_core_headers
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILE_SET scry_generated_headers
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(
  EXPORT scryTargets
  FILE scryTargets.cmake
  NAMESPACE scry::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/scry"
)
install(
  FILES
    "${PROJECT_BINARY_DIR}/scryConfig.cmake"
    "${PROJECT_BINARY_DIR}/scryConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/scry"
)
install(
  FILES LICENSE
  DESTINATION "${CMAKE_INSTALL_DOCDIR}"
)
