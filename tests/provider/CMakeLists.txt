add_executable(
  scry_provider_tests
  anthropic_edge_tests.cpp
  json_accessor_tests.cpp
  openai_request_tests.cpp
  openai_stream_tests.cpp
  provider_tests.cpp
  request_encoding_tests.cpp
  stream_tests.cpp
  tool_boundary_tests.cpp
)
target_include_directories(
  scry_provider_tests
  PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_include_directories(
  scry_provider_tests
  SYSTEM PRIVATE "${SCRY_GLAZE_INCLUDE_DIR}"
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
