add_executable(
  scry_transport_tests
  curl_transport_tests.cpp
  loopback_server.cpp
  transport_policy_tests.cpp
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
    scry::testing
    scry_project_options
)
catch_discover_tests(
  scry_transport_tests
  TEST_PREFIX "transport."
  PROPERTIES TIMEOUT 15
)
