add_executable(scry_sse_tests sse_tests.cpp)
target_include_directories(scry_sse_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(
  scry_sse_tests
  PRIVATE Catch2::Catch2WithMain scry::scry scry_project_options
)
catch_discover_tests(scry_sse_tests TEST_PREFIX "protocol.")

if(SCRY_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "SCRY_BUILD_FUZZERS requires a Clang-family compiler")
  endif()
  add_executable(
    scry_sse_fuzz
    sse_fuzz.cpp
    "${PROJECT_SOURCE_DIR}/src/protocol/sse.cpp"
  )
  target_include_directories(scry_sse_fuzz PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_compile_options(scry_sse_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_options(scry_sse_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_libraries(scry_sse_fuzz PRIVATE scry_project_options)
  file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/fuzz-corpus/sse")
  add_test(
    NAME protocol.sse-fuzz
    COMMAND
      scry_sse_fuzz
      -runs=1000
      "${PROJECT_BINARY_DIR}/fuzz-corpus/sse"
      "${PROJECT_SOURCE_DIR}/tests/fixtures/anthropic"
  )
endif()
