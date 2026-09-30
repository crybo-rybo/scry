# The turn machine includes the public message model, whose blocks carry P3394
# annotations, so its tests are C++26 and take the machine from scry itself.
add_executable(
  scry_turn_machine_tests
  retry_tests.cpp
  tool_budget_tests.cpp
  tool_loop_tests.cpp
  turn_machine_tests.cpp
  typed_answer_tests.cpp
)
target_include_directories(
  scry_turn_machine_tests
  PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(
  scry_turn_machine_tests
  PRIVATE Catch2::Catch2WithMain scry::scry scry_project_options
)
catch_discover_tests(scry_turn_machine_tests TEST_PREFIX "machine.")
