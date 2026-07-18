add_executable(
  scry_harness_integration_tests
  conversation_persistence_tests.cpp
  curl_harness_tests.cpp
  harness_tests.cpp
  runtime_coverage_tests.cpp
  runtime_edge_tests.cpp
  tool_loop_tests.cpp
  "${PROJECT_SOURCE_DIR}/tests/support/transport/loopback_server.cpp"
)
target_include_directories(
  scry_harness_integration_tests
  PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
)
target_link_libraries(
  scry_harness_integration_tests
  PRIVATE Catch2::Catch2WithMain scry::scry scry_project_options
)
catch_discover_tests(
  scry_harness_integration_tests
  TEST_PREFIX "integration."
  PROPERTIES TIMEOUT 15
)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_executable(scry_tls_probe tls_probe.cpp)
target_link_libraries(
  scry_tls_probe
  PRIVATE scry::scry scry_project_options
)
add_test(
  NAME "integration.self-signed TLS is rejected unless explicitly disabled"
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tls_server_test.py"
    --probe "$<TARGET_FILE:scry_tls_probe>"
    --cert "${PROJECT_SOURCE_DIR}/tests/fixtures/tls/self-signed-cert.pem"
    --key "${PROJECT_SOURCE_DIR}/tests/fixtures/tls/self-signed-key.pem"
)
set_tests_properties(
  "integration.self-signed TLS is rejected unless explicitly disabled"
  PROPERTIES TIMEOUT 15
)
