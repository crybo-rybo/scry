foreach(SCRY_EXAMPLE IN ITEMS quickstart main_loop tools typed_answer)
  add_executable(scry_${SCRY_EXAMPLE}_example ${SCRY_EXAMPLE}.cpp)
  target_link_libraries(
    scry_${SCRY_EXAMPLE}_example
    PRIVATE scry::scry scry_project_options
  )
endforeach()

# Needs no model, so tests/testing runs it under ctest.
if(SCRY_BUILD_TESTING_SUPPORT)
  add_executable(scry_scripted_testing_example testing_scripted.cpp)
  target_link_libraries(
    scry_scripted_testing_example
    PRIVATE scry::testing scry_project_options
  )
endif()
