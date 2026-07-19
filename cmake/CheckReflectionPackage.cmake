if(NOT DEFINED SCRY_PACKAGE_PREFIX)
  message(FATAL_ERROR "SCRY_PACKAGE_PREFIX is required")
endif()

set(
  SCRY_REFLECTION_EXPORT
  "${SCRY_PACKAGE_PREFIX}/lib/cmake/scry/scryReflectionTargets.cmake"
)
set(
  SCRY_CORE_EXPORT
  "${SCRY_PACKAGE_PREFIX}/lib/cmake/scry/scryTargets.cmake"
)
set(
  SCRY_REQUIRED_REFLECTION_FILES
  "${SCRY_PACKAGE_PREFIX}/include/scry/reflection.hpp"
  "${SCRY_PACKAGE_PREFIX}/include/scry/detail/reflection_json.hpp"
  "${SCRY_PACKAGE_PREFIX}/lib/libscry_reflection.a"
  "${SCRY_REFLECTION_EXPORT}"
)

foreach(SCRY_REQUIRED_FILE IN LISTS SCRY_REQUIRED_REFLECTION_FILES)
  if(NOT EXISTS "${SCRY_REQUIRED_FILE}")
    message(FATAL_ERROR "Reflection package file is missing: ${SCRY_REQUIRED_FILE}")
  endif()
endforeach()

file(READ "${SCRY_REFLECTION_EXPORT}" SCRY_REFLECTION_EXPORT_CONTENTS)
foreach(
  SCRY_FORBIDDEN_EXPORT_TEXT
  IN ITEMS
    "glaze"
    "scry_project_options"
    "fexpansion-statements"
    "stdlib=libc++"
)
  string(
    FIND
    "${SCRY_REFLECTION_EXPORT_CONTENTS}"
    "${SCRY_FORBIDDEN_EXPORT_TEXT}"
    SCRY_FORBIDDEN_EXPORT_OFFSET
  )
  if(NOT SCRY_FORBIDDEN_EXPORT_OFFSET EQUAL -1)
    message(
      FATAL_ERROR
      "Private build detail leaked into the reflection export: ${SCRY_FORBIDDEN_EXPORT_TEXT}"
    )
  endif()
endforeach()

if(
  NOT SCRY_REFLECTION_EXPORT_CONTENTS
  MATCHES "INTERFACE_COMPILE_OPTIONS \"-std=c\\+\\+26;-freflection\""
)
  message(FATAL_ERROR "Reflection export is missing its supported compiler flags")
endif()

file(READ "${SCRY_CORE_EXPORT}" SCRY_CORE_EXPORT_CONTENTS)
if(SCRY_CORE_EXPORT_CONTENTS MATCHES "scry::reflection")
  message(FATAL_ERROR "The reflection target leaked into the core export")
endif()

message(STATUS "Installed reflection package audit passed")
