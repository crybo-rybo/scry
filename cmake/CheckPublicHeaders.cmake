if(NOT DEFINED SCRY_PUBLIC_INCLUDE_DIR)
  message(FATAL_ERROR "SCRY_PUBLIC_INCLUDE_DIR is required")
endif()

file(
  GLOB_RECURSE
  SCRY_AUDITED_HEADERS
  LIST_DIRECTORIES FALSE
  "${SCRY_PUBLIC_INCLUDE_DIR}/scry/*.hpp"
)

if(NOT SCRY_AUDITED_HEADERS)
  message(FATAL_ERROR "No public Scry headers were found")
endif()

foreach(SCRY_HEADER IN LISTS SCRY_AUDITED_HEADERS)
  file(READ "${SCRY_HEADER}" SCRY_HEADER_CONTENTS)

  if(
    SCRY_HEADER_CONTENTS
    MATCHES "#[ \t]*include[ \t]*[<\"](curl|glaze)[^>\"]*[>\"]"
  )
    message(FATAL_ERROR "Third-party include leaked into ${SCRY_HEADER}")
  endif()

  if(SCRY_HEADER_CONTENTS MATCHES "(^|[\n \t])(virtual|protected)([\n \t:]|$)")
    message(FATAL_ERROR "Inheritable public surface found in ${SCRY_HEADER}")
  endif()
endforeach()

message(STATUS "Public-header audit passed")
