add_executable(
  scry_provider_tests
  coverage_tests.cpp
  provider_tests.cpp
  stream_tests.cpp
  tool_boundary_tests.cpp
)
target_include_directories(
  scry_provider_tests
  PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_include_directories(
  scry_provider_tests
  SYSTEM PRIVATE "${glaze_SOURCE_DIR}/include"
)
target_compile_definitions(
  scry_provider_tests
  PRIVATE
    "SCRY_ANTHROPIC_FIXTURE_DIR=\"${PROJECT_SOURCE_DIR}/tests/fixtures/anthropic\""
)
target_link_libraries(
  scry_provider_tests
  PRIVATE Catch2::Catch2WithMain scry::scry scry_project_options
)
catch_discover_tests(scry_provider_tests TEST_PREFIX "provider.")

if(SCRY_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "SCRY_BUILD_FUZZERS requires a Clang-family compiler")
  endif()
  add_executable(
    scry_anthropic_fuzz
    anthropic_fuzz.cpp
    "${PROJECT_SOURCE_DIR}/src/provider/anthropic_content.cpp"
    "${PROJECT_SOURCE_DIR}/src/provider/anthropic_request.cpp"
    "${PROJECT_SOURCE_DIR}/src/provider/anthropic_response.cpp"
    "${PROJECT_SOURCE_DIR}/src/provider/anthropic_stream.cpp"
    "${PROJECT_SOURCE_DIR}/src/provider/provider.cpp"
    "${PROJECT_SOURCE_DIR}/src/provider/wire_json.cpp"
  )
  target_compile_features(scry_anthropic_fuzz PRIVATE cxx_std_23)
  target_include_directories(
    scry_anthropic_fuzz
    PRIVATE "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src"
  )
  target_include_directories(
    scry_anthropic_fuzz
    SYSTEM PRIVATE "${glaze_SOURCE_DIR}/include"
  )
  target_compile_options(scry_anthropic_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_options(scry_anthropic_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_libraries(scry_anthropic_fuzz PRIVATE scry_project_options)
  file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/fuzz-corpus/anthropic")
  add_test(
    NAME provider.anthropic-fuzz
    COMMAND
      scry_anthropic_fuzz
      -runs=1000
      -timeout=5
      -max_total_time=30
      "${PROJECT_BINARY_DIR}/fuzz-corpus/anthropic"
      "${PROJECT_SOURCE_DIR}/tests/fuzz/corpus/anthropic"
  )
  set_tests_properties(provider.anthropic-fuzz PROPERTIES TIMEOUT 45)
endif()
