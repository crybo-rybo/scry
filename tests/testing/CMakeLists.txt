# This suite is a consumer of scry::testing: it reaches the runtime only through
# the installed public surface, so it links no private include directory.
add_executable(scry_testing_tests scripted_transport_tests.cpp)
target_link_libraries(
  scry_testing_tests
  PRIVATE
    Catch2::Catch2WithMain
    scry::testing
    scry_project_options
)
catch_discover_tests(
  scry_testing_tests
  TEST_PREFIX "testing."
  PROPERTIES TIMEOUT 15
)

# examples/ is configured before tests/, so the consumer-shaped example is
# already a target here when examples are enabled; its exit code is the result.
if(TARGET scry_scripted_testing_example)
  add_test(
    NAME "testing.scripted consumer example"
    COMMAND scry_scripted_testing_example
  )
  set_tests_properties(
    "testing.scripted consumer example"
    PROPERTIES TIMEOUT 15
  )
endif()
