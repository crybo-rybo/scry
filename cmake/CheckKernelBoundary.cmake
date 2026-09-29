# cmake -DSCRY_SOURCE_DIR=<src> -P CheckKernelBoundary.cmake
#
# The kernel (src/kernel/) is compiled as C++23 without reflection, and the rest
# of src/ may use reflection, so kernel code must not reach outside itself. This
# audit fails when a kernel file
#   - includes a public header other than the allowlisted ones below,
#   - includes a src/ header outside src/kernel/,
#   - is a source file whose first include is not "kernel/kernel.hpp", the
#     header holding the reflection guard, or
#   - is a header that includes "kernel/kernel.hpp": C++26 code includes kernel
#     headers, and the guard would reject it.
# Compiling the kernel as C++23 is the real enforcement for reflection syntax;
# this audit keeps the dependency direction explicit and cheap to check.

set(
  SCRY_KERNEL_PUBLIC_HEADERS
  scry/config.hpp
  scry/error.hpp
  scry/json.hpp
  scry/turn_id.hpp
  scry/unique_function.hpp
)

if(NOT SCRY_SOURCE_DIR OR NOT IS_DIRECTORY "${SCRY_SOURCE_DIR}/kernel")
  message(FATAL_ERROR "Pass -DSCRY_SOURCE_DIR=<the src/ directory>")
endif()

# Every top-level src/ directory other than kernel/ names a non-kernel layer.
file(GLOB SCRY_SOURCE_ENTRIES LIST_DIRECTORIES TRUE "${SCRY_SOURCE_DIR}/*")
set(SCRY_OUTSIDE_LAYERS "")
foreach(SCRY_ENTRY IN LISTS SCRY_SOURCE_ENTRIES)
  get_filename_component(SCRY_ENTRY_NAME "${SCRY_ENTRY}" NAME)
  if(IS_DIRECTORY "${SCRY_ENTRY}" AND NOT SCRY_ENTRY_NAME STREQUAL "kernel")
    list(APPEND SCRY_OUTSIDE_LAYERS "${SCRY_ENTRY_NAME}")
  endif()
endforeach()

file(
  GLOB_RECURSE
  SCRY_KERNEL_FILES
  LIST_DIRECTORIES FALSE
  "${SCRY_SOURCE_DIR}/kernel/*.hpp"
  "${SCRY_SOURCE_DIR}/kernel/*.cpp"
)
if(NOT SCRY_KERNEL_FILES)
  message(FATAL_ERROR "No kernel sources were found under ${SCRY_SOURCE_DIR}/kernel")
endif()

set(SCRY_VIOLATIONS "")
foreach(SCRY_FILE IN LISTS SCRY_KERNEL_FILES)
  file(RELATIVE_PATH SCRY_NAME "${SCRY_SOURCE_DIR}" "${SCRY_FILE}")
  file(
    STRINGS "${SCRY_FILE}" SCRY_INCLUDE_LINES
    REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"][^>\"]+[>\"]"
  )
  set(SCRY_FIRST_INCLUDE "")
  set(SCRY_HAS_GUARD FALSE)
  foreach(SCRY_LINE IN LISTS SCRY_INCLUDE_LINES)
    string(
      REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]*[<\"]([^>\"]+)[>\"].*$" "\\1"
      SCRY_INCLUDED "${SCRY_LINE}"
    )
    if(NOT SCRY_FIRST_INCLUDE)
      set(SCRY_FIRST_INCLUDE "${SCRY_INCLUDED}")
    endif()
    if(SCRY_INCLUDED STREQUAL "kernel/kernel.hpp")
      set(SCRY_HAS_GUARD TRUE)
    endif()
    string(REGEX MATCH "^[^/]+" SCRY_FIRST_COMPONENT "${SCRY_INCLUDED}")
    if(SCRY_FIRST_COMPONENT STREQUAL "scry")
      if(NOT SCRY_INCLUDED IN_LIST SCRY_KERNEL_PUBLIC_HEADERS)
        list(APPEND SCRY_VIOLATIONS "${SCRY_NAME}: public header <${SCRY_INCLUDED}>")
      endif()
    elseif(
      SCRY_FIRST_COMPONENT IN_LIST SCRY_OUTSIDE_LAYERS
      AND SCRY_INCLUDED MATCHES "/"
    )
      list(APPEND SCRY_VIOLATIONS "${SCRY_NAME}: non-kernel header \"${SCRY_INCLUDED}\"")
    endif()
  endforeach()
  if(SCRY_NAME MATCHES "\\.cpp$" AND NOT SCRY_FIRST_INCLUDE STREQUAL "kernel/kernel.hpp")
    list(
      APPEND SCRY_VIOLATIONS
      "${SCRY_NAME}: first include is not \"kernel/kernel.hpp\""
    )
  elseif(SCRY_NAME MATCHES "\\.hpp$" AND SCRY_HAS_GUARD)
    list(
      APPEND SCRY_VIOLATIONS
      "${SCRY_NAME}: a header must not include \"kernel/kernel.hpp\""
    )
  endif()
endforeach()

if(SCRY_VIOLATIONS)
  list(JOIN SCRY_VIOLATIONS "\n  " SCRY_REPORT)
  message(FATAL_ERROR "Kernel include boundary violated:\n  ${SCRY_REPORT}")
endif()

list(LENGTH SCRY_KERNEL_FILES SCRY_KERNEL_FILE_COUNT)
message(STATUS "Kernel include boundary holds for ${SCRY_KERNEL_FILE_COUNT} files")
