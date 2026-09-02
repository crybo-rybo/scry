add_executable(
  scry_sse_tests
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
  scry_require_fuzzer_compiler()
  set(SCRY_FUZZ_TEST_PREFIX "protocol.")
  scry_add_fuzzer(
    scry_sse_fuzz
    sse
    SOURCES sse_fuzz.cpp "${PROJECT_SOURCE_DIR}/src/protocol/sse.cpp"
  )
endif()
