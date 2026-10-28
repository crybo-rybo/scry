# Every public header must compile on its own, so each one becomes a lone
# translation unit that includes nothing else.
file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/generated/header_checks")
add_library(scrydantic_header_checks OBJECT)
# Keep one translation unit per header even in a unity build.
set_target_properties(scrydantic_header_checks PROPERTIES UNITY_BUILD OFF)
target_link_libraries(
  scrydantic_header_checks
  PRIVATE scrydantic::scrydantic scrydantic_project_options
)
foreach(SCRYDANTIC_HEADER IN LISTS SCRYDANTIC_PUBLIC_HEADERS)
  string(REGEX REPLACE "^include/" "" SCRYDANTIC_HEADER_INCLUDE "${SCRYDANTIC_HEADER}")
  string(MAKE_C_IDENTIFIER "${SCRYDANTIC_HEADER_INCLUDE}" SCRYDANTIC_HEADER_ID)
  set(
    SCRYDANTIC_HEADER_CHECK_SOURCE
    "${PROJECT_BINARY_DIR}/generated/header_checks/${SCRYDANTIC_HEADER_ID}.cpp"
  )
  file(
    GENERATE
    OUTPUT "${SCRYDANTIC_HEADER_CHECK_SOURCE}"
    CONTENT "#include <${SCRYDANTIC_HEADER_INCLUDE}>\n"
  )
  target_sources(scrydantic_header_checks PRIVATE "${SCRYDANTIC_HEADER_CHECK_SOURCE}")
endforeach()
