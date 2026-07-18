add_executable(
  scry_transport_tests
  curl_transport_tests.cpp
  fake_transport_tests.cpp
  transport_coverage_tests.cpp
  transport_policy_tests.cpp
  "${PROJECT_SOURCE_DIR}/tests/support/transport/loopback_server.cpp"
)
target_include_directories(
  scry_transport_tests
  PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
)
target_link_libraries(
  scry_transport_tests
  PRIVATE
    Catch2::Catch2WithMain
    scry::scry
    scry_project_options
)
catch_discover_tests(
  scry_transport_tests
  TEST_PREFIX "transport."
  PROPERTIES TIMEOUT 15
)
