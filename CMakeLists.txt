# 3.30 is the first CMake that knows GCC's C++26 mode (cxx_std_26 below).
cmake_minimum_required(VERSION 3.30)

project(
  scry
  VERSION 0.6.0
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
option(SCRY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ${PROJECT_IS_TOP_LEVEL})
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

# The public API uses C++26 reflection (P2996), which only GCC 16+ compiles.
if(
  NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
  OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16
)
  message(FATAL_ERROR "Scry requires GCC 16 or newer (C++26 reflection)")
endif()

find_package(CURL 7.84 REQUIRED)
find_package(Threads REQUIRED)

# Warning and sanitizer flags for Scry's own targets; never exported.
add_library(scry_project_options INTERFACE)
target_compile_options(
  scry_project_options
  INTERFACE -Wall -Wextra -Wconversion -Wshadow
)
if(SCRY_WARNINGS_AS_ERRORS)
  target_compile_options(scry_project_options INTERFACE -Werror)
endif()
if(SCRY_SANITIZER STREQUAL "address-undefined")
  set(SCRY_SANITIZER_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined)
elseif(SCRY_SANITIZER STREQUAL "thread")
  set(SCRY_SANITIZER_FLAGS -fsanitize=thread)
elseif(NOT SCRY_SANITIZER STREQUAL "none")
  message(FATAL_ERROR "Unsupported SCRY_SANITIZER: ${SCRY_SANITIZER}")
endif()
if(SCRY_SANITIZER_FLAGS)
  target_compile_options(
    scry_project_options
    INTERFACE ${SCRY_SANITIZER_FLAGS} -fno-omit-frame-pointer
  )
  target_link_options(scry_project_options INTERFACE ${SCRY_SANITIZER_FLAGS})
endif()

# The kernel is the reflection-free layer under the library: the JSON codec, the
# SSE parser, retry delay arithmetic, and the transport seam with its libcurl
# implementation, which is everything that parses untrusted bytes. It is compiled
# as C++23 without -freflection, so plain clang-tidy can analyze it
# (scripts/lint.sh). It never links scry, so scry's PUBLIC C++26 and -freflection
# cannot reach it. src/kernel/kernel.hpp states the include boundary.
add_library(
  scry_kernel
  OBJECT
    src/kernel/error.cpp
    src/kernel/json/codec.cpp
    src/kernel/json/document.cpp
    src/kernel/json/number.cpp
    src/kernel/json/view.cpp
    src/kernel/retry.cpp
    src/kernel/sse.cpp
    src/kernel/transport/curl_error.cpp
    src/kernel/transport/curl_global.cpp
    src/kernel/transport/curl_transport.cpp
    src/kernel/transport/transport_policy.cpp
)
target_compile_features(scry_kernel PRIVATE cxx_std_23)
# In C++23 mode GCC rejects a reflection operator outright but accepts a P3394
# annotation with only a -Wc++26-extensions warning, so that warning is an error
# for the kernel even when SCRY_WARNINGS_AS_ERRORS is off.
target_compile_options(scry_kernel PRIVATE -Werror=c++26-extensions)
set_target_properties(
  scry_kernel
  PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(
  scry_kernel
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
)
# A target that links the kernel objects directly, such as a kernel test, also
# needs libcurl and Threads at link time. scry takes the objects through
# $<TARGET_OBJECTS:...> instead and declares both dependencies itself.
target_link_libraries(
  scry_kernel
  PUBLIC CURL::libcurl Threads::Threads
  PRIVATE scry_project_options
)

add_library(
  scry
  STATIC
    src/machine/turn_machine.cpp
    src/provider/anthropic_content.cpp
    src/provider/anthropic_request.cpp
    src/provider/anthropic_stream.cpp
    src/provider/openai_request.cpp
    src/provider/openai_stream.cpp
    src/provider/provider.cpp
    src/reflection/json_bridge.cpp
    src/runtime/config.cpp
    src/runtime/conversation.cpp
    src/runtime/conversation_persistence.cpp
    src/runtime/harness.cpp
    src/runtime/pump.cpp
    src/runtime/queue.cpp
    src/runtime/tool_dispatch.cpp
    src/runtime/tool_registry.cpp
    src/runtime/turn.cpp
    src/runtime/turn_route.cpp
    src/runtime/worker.cpp
)
# The kernel's objects are archived into scry as sources rather than linked as a
# target, so scry_kernel never becomes a dependency of the installed export set.
target_sources(scry PRIVATE $<TARGET_OBJECTS:scry_kernel>)
add_library(scry::scry ALIAS scry)
set_target_properties(
  scry
  PROPERTIES
    EXPORT_NAME scry
    POSITION_INDEPENDENT_CODE ON
    CXX_EXTENSIONS OFF
)
target_compile_features(scry PUBLIC cxx_std_26)
target_compile_options(scry PUBLIC -freflection)
target_include_directories(
  scry
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${SCRY_GENERATED_INCLUDE_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(
  scry
  PRIVATE
    CURL::libcurl
    Threads::Threads
    "$<BUILD_INTERFACE:scry_project_options>"
)

set(
  SCRY_CORE_PUBLIC_HEADERS
  include/scry/annotations.hpp
  include/scry/config.hpp
  include/scry/conversation.hpp
  include/scry/detail/reflection_codec.hpp
  include/scry/detail/reflection_decode.hpp
  include/scry/detail/reflection_encode.hpp
  include/scry/detail/reflection_json.hpp
  include/scry/detail/reflection_json_string.hpp
  include/scry/detail/reflection_meta.hpp
  include/scry/detail/reflection_model.hpp
  include/scry/detail/reflection_registration.hpp
  include/scry/detail/reflection_schema.hpp
  include/scry/detail/reflection_tools.hpp
  include/scry/error.hpp
  include/scry/events.hpp
  include/scry/harness.hpp
  include/scry/json.hpp
  include/scry/message.hpp
  include/scry/reflection.hpp
  include/scry/scry.hpp
  include/scry/tool.hpp
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

# CTest is enabled from this top-level file so ctest runs from the build root.
if(SCRY_BUILD_TESTS)
  include(CTest)
  add_subdirectory(tests)
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
