add_executable(scry_canonical_example main_loop.cpp)
target_link_libraries(
  scry_canonical_example
  PRIVATE scry::scry scry_project_options
)
