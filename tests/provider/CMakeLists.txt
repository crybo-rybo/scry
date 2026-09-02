if(SCRY_BUILD_FUZZERS)
  scry_require_fuzzer_compiler()
  set(SCRY_FUZZ_TEST_PREFIX "provider.")
  set(
    scry_provider_fuzz_sources
    "${PROJECT_SOURCE_DIR}/src/core/error.cpp"
    "${PROJECT_SOURCE_DIR}/src/core/json_codec.cpp"
    "${PROJECT_SOURCE_DIR}/src/provider/anthropic_content.cpp"
    "${PROJECT_SOURCE_DIR}/src/provider/anthropic_request.cpp"
    "${PROJECT_SOURCE_DIR}/src/provider/anthropic_stream.cpp"
    "${PROJECT_SOURCE_DIR}/src/provider/openai_content.cpp"
    "${PROJECT_SOURCE_DIR}/src/provider/openai_request.cpp"
    "${PROJECT_SOURCE_DIR}/src/provider/openai_stream.cpp"
    "${PROJECT_SOURCE_DIR}/src/provider/provider.cpp"
  )
  scry_add_fuzzer(
    scry_anthropic_fuzz
    anthropic
    SOURCES anthropic_fuzz.cpp ${scry_provider_fuzz_sources}
  )
  scry_add_fuzzer(
    scry_openai_fuzz
    openai
    SOURCES openai_fuzz.cpp ${scry_provider_fuzz_sources}
  )
endif()

# SCRY_CLANG_TOOLING contributes only the fuzz targets from this directory; the
# Catch2 suite below reaches the C++26 public API and cannot compile under Clang.
if(SCRY_CLANG_TOOLING)
  return()
endif()

add_executable(
  scry_provider_tests
  anthropic_edge_tests.cpp
  json_accessor_tests.cpp
  openai_request_tests.cpp
  openai_stream_tests.cpp
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
    "SCRY_OPENAI_FIXTURE_DIR=\"${PROJECT_SOURCE_DIR}/tests/fixtures/openai\""
)
target_link_libraries(
  scry_provider_tests
  PRIVATE Catch2::Catch2WithMain scry::scry scry_project_options
)
catch_discover_tests(scry_provider_tests TEST_PREFIX "provider.")
