add_executable(
  scry_turn_machine_tests
  turn_machine_tests.cpp
  "${PROJECT_SOURCE_DIR}/src/core/retry.cpp"
  "${PROJECT_SOURCE_DIR}/src/machine/turn_machine.cpp"
)
target_compile_features(scry_turn_machine_tests PRIVATE cxx_std_23)
target_include_directories(
  scry_turn_machine_tests
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(
  scry_turn_machine_tests
  PRIVATE Catch2::Catch2WithMain scry_project_options
)
catch_discover_tests(scry_turn_machine_tests TEST_PREFIX "machine.")
