add_executable(
  scry_backend_tests
  http_backend_tests.cpp
  model_backend_tests.cpp
)
target_include_directories(
  scry_backend_tests
  PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
)
target_link_libraries(
  scry_backend_tests
  PRIVATE Catch2::Catch2WithMain scry::scry scry_project_options
)
catch_discover_tests(
  scry_backend_tests
  TEST_PREFIX "backend."
  PROPERTIES TIMEOUT 15
)
