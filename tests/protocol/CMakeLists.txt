add_executable(
  scry_sse_tests
  sse_coverage_tests.cpp
  sse_tests.cpp
  "${PROJECT_SOURCE_DIR}/src/protocol/sse.cpp"
)
target_compile_features(scry_sse_tests PRIVATE cxx_std_23)
target_include_directories(
  scry_sse_tests
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(
  scry_sse_tests
  PRIVATE Catch2::Catch2WithMain scry_project_options
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
  target_compile_features(scry_sse_fuzz PRIVATE cxx_std_23)
  target_include_directories(
    scry_sse_fuzz
    PRIVATE "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src"
  )
  target_compile_options(scry_sse_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_options(scry_sse_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_libraries(scry_sse_fuzz PRIVATE scry_project_options)
  file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/fuzz-corpus/sse")
  add_test(
    NAME protocol.sse-fuzz
    COMMAND
      scry_sse_fuzz
      -runs=1000
      -timeout=5
      -max_total_time=30
      "${PROJECT_BINARY_DIR}/fuzz-corpus/sse"
      "${PROJECT_SOURCE_DIR}/tests/fuzz/corpus/sse"
  )
  set_tests_properties(protocol.sse-fuzz PROPERTIES TIMEOUT 45)
endif()
