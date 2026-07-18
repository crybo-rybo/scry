add_executable(scry_turn_machine_tests turn_machine_tests.cpp)
target_include_directories(
  scry_turn_machine_tests
  PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(
  scry_turn_machine_tests
  PRIVATE Catch2::Catch2WithMain scry::scry scry_project_options
)
catch_discover_tests(scry_turn_machine_tests TEST_PREFIX "machine.")
