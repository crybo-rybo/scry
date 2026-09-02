cmake_minimum_required(VERSION 3.25)

project(scry_docs LANGUAGES NONE)

get_filename_component(SCRY_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

file(READ "${SCRY_ROOT_DIR}/include/scry/version.hpp" SCRY_VERSION_HEADER)
foreach(SCRY_VERSION_PART MAJOR MINOR PATCH)
  if(NOT SCRY_VERSION_HEADER MATCHES "#define SCRY_VERSION_${SCRY_VERSION_PART} ([0-9]+)")
    message(FATAL_ERROR "Could not determine SCRY_VERSION_${SCRY_VERSION_PART}")
  endif()
  set("SCRY_DOCS_VERSION_${SCRY_VERSION_PART}" "${CMAKE_MATCH_1}")
endforeach()
set(
  SCRY_DOCS_VERSION
  "${SCRY_DOCS_VERSION_MAJOR}.${SCRY_DOCS_VERSION_MINOR}.${SCRY_DOCS_VERSION_PATCH}"
)

# Ubuntu 24.04, the documentation CI host, provides Doxygen 1.9.8. Keeping
# that version as the floor preserves compatibility with the hosted build.
find_package(Doxygen 1.9.8 REQUIRED COMPONENTS doxygen dot)

# Keep the documented code surface explicit: installed/public headers live under
# include/, while contributor-facing implementation documentation lives under
# src/. Tests, benchmarks, examples, and build tooling are intentionally outside
# Doxygen's code input even when they contain C++.
file(
  GLOB_RECURSE SCRY_DOCUMENTED_PUBLIC_HEADERS
  CONFIGURE_DEPENDS
  LIST_DIRECTORIES false
  "${SCRY_ROOT_DIR}/include/*.hpp"
)
file(
  GLOB_RECURSE SCRY_DOCUMENTED_IMPLEMENTATION_FILES
  CONFIGURE_DEPENDS
  LIST_DIRECTORIES false
  "${SCRY_ROOT_DIR}/src/*.hpp"
  "${SCRY_ROOT_DIR}/src/*.cpp"
)
set(
  SCRY_DOCUMENTED_CODE
  ${SCRY_DOCUMENTED_PUBLIC_HEADERS}
  ${SCRY_DOCUMENTED_IMPLEMENTATION_FILES}
)
list(SORT SCRY_DOCUMENTED_CODE)
list(TRANSFORM SCRY_DOCUMENTED_CODE PREPEND "\"")
list(TRANSFORM SCRY_DOCUMENTED_CODE APPEND "\"")
list(JOIN SCRY_DOCUMENTED_CODE " " SCRY_DOXYGEN_INPUT)

configure_file(Doxyfile.in Doxyfile @ONLY)

add_custom_target(
  scry_docs
  COMMAND "${CMAKE_COMMAND}" -E remove_directory "${CMAKE_CURRENT_BINARY_DIR}/html"
  COMMAND "${DOXYGEN_EXECUTABLE}" "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
  BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/html/index.html"
  WORKING_DIRECTORY "${SCRY_ROOT_DIR}"
  COMMENT "Building Scry API and source documentation"
  VERBATIM
)
