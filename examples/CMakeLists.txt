add_executable(scry_canonical_example main_loop.cpp)
target_link_libraries(
  scry_canonical_example
  PRIVATE scry::scry scry_project_options
)

# A consumer-shaped test built on the installed scry::testing component. It
# takes no test framework and reports through its exit code, so tests/ can run
# it directly under ctest.
if(SCRY_BUILD_TESTING_SUPPORT)
  add_executable(scry_scripted_testing_example testing_scripted.cpp)
  target_link_libraries(
    scry_scripted_testing_example
    PRIVATE scry::testing scry_project_options
  )
endif()

add_executable(scry_tool_policy_example tool_policy.cpp)
target_link_libraries(
  scry_tool_policy_example
  PRIVATE scry::scry scry_project_options
)
