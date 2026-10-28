# Scrydantic builds on its own (cmake -S scrydantic) or as a subdirectory of
# Scry. Nothing here may reach outside this directory: scripts/lint.sh rejects
# an include of a Scry header, and no target links libcurl or Threads.
cmake_minimum_required(VERSION 3.30)

project(
  scrydantic
  VERSION 0.1.0
  DESCRIPTION "C++26 reflection to JSON Schema, a strict JSON codec, and reflected tools"
  LANGUAGES CXX
)

# Scrydantic uses no C++ modules; see the root CMakeLists.txt of Scry for why
# scanning stays off.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

option(SCRYDANTIC_BUILD_TESTS "Build Scrydantic's tests" ${PROJECT_IS_TOP_LEVEL})
option(
  SCRYDANTIC_BUILD_EXAMPLES
  "Compile Scrydantic's examples"
  ${PROJECT_IS_TOP_LEVEL}
)
option(
  SCRYDANTIC_WARNINGS_AS_ERRORS
  "Treat compiler warnings as errors"
  ${PROJECT_IS_TOP_LEVEL}
)
set(
  SCRYDANTIC_SANITIZER
  "none"
  CACHE STRING
  "Sanitizer mode: none, address-undefined, or thread"
)
set_property(
  CACHE SCRYDANTIC_SANITIZER
  PROPERTY STRINGS none address-undefined thread
)

# The reflected codec uses C++26 reflection (P2996), which only GCC 16+ compiles.
if(
  NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
  OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16
)
  message(FATAL_ERROR "Scrydantic requires GCC 16 or newer (C++26 reflection)")
endif()

# Scry's package configuration names the Scrydantic version it was built with.
if(NOT PROJECT_IS_TOP_LEVEL)
  set(SCRYDANTIC_VERSION "${PROJECT_VERSION}" PARENT_SCOPE)
endif()

# Warning and sanitizer flags for Scrydantic's own targets; never exported.
add_library(scrydantic_project_options INTERFACE)
target_compile_options(
  scrydantic_project_options
  INTERFACE -Wall -Wextra -Wconversion -Wshadow
)
if(SCRYDANTIC_WARNINGS_AS_ERRORS)
  target_compile_options(scrydantic_project_options INTERFACE -Werror)
endif()
if(SCRYDANTIC_SANITIZER STREQUAL "address-undefined")
  set(
    SCRYDANTIC_SANITIZER_FLAGS
    -fsanitize=address,undefined -fno-sanitize-recover=undefined
  )
elseif(SCRYDANTIC_SANITIZER STREQUAL "thread")
  set(SCRYDANTIC_SANITIZER_FLAGS -fsanitize=thread)
elseif(NOT SCRYDANTIC_SANITIZER STREQUAL "none")
  message(FATAL_ERROR "Unsupported SCRYDANTIC_SANITIZER: ${SCRYDANTIC_SANITIZER}")
endif()
if(SCRYDANTIC_SANITIZER_FLAGS)
  target_compile_options(
    scrydantic_project_options
    INTERFACE ${SCRYDANTIC_SANITIZER_FLAGS} -fno-omit-frame-pointer
  )
  target_link_options(
    scrydantic_project_options
    INTERFACE ${SCRYDANTIC_SANITIZER_FLAGS}
  )
endif()

# The reflected codec, schema generator, and tool binding: header-only C++26
# with reflection, so its consumers compile as C++26 with -freflection too.
add_library(scrydantic INTERFACE)
add_library(scrydantic::scrydantic ALIAS scrydantic)
set_target_properties(scrydantic PROPERTIES EXPORT_NAME scrydantic)
target_compile_features(scrydantic INTERFACE cxx_std_26)
target_compile_options(scrydantic INTERFACE -freflection)
target_include_directories(
  scrydantic
  INTERFACE
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

set(
  SCRYDANTIC_PUBLIC_HEADERS
  include/scrydantic/scrydantic.hpp
)
target_sources(
  scrydantic
  INTERFACE
    FILE_SET scrydantic_headers
    TYPE HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/include"
    FILES ${SCRYDANTIC_PUBLIC_HEADERS}
)

# CTest is enabled here when Scrydantic is the top-level project; under Scry,
# the root CMakeLists.txt enables it before adding this directory.
if(SCRYDANTIC_BUILD_TESTS)
  if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
  endif()
  add_subdirectory(tests)
endif()

configure_package_config_file(
  cmake/scrydanticConfig.cmake.in
  "${PROJECT_BINARY_DIR}/scrydanticConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/scrydantic"
)
# Pre-1.0, minor releases may break API, as for Scry.
write_basic_package_version_file(
  "${PROJECT_BINARY_DIR}/scrydanticConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMinorVersion
)

install(
  TARGETS scrydantic
  EXPORT scrydanticTargets
  FILE_SET scrydantic_headers
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(
  EXPORT scrydanticTargets
  FILE scrydanticTargets.cmake
  NAMESPACE scrydantic::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/scrydantic"
)
install(
  FILES
    "${PROJECT_BINARY_DIR}/scrydanticConfig.cmake"
    "${PROJECT_BINARY_DIR}/scrydanticConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/scrydantic"
)
