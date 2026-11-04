# A consumer of Scrydantic alone: it publishes reflected tools as an MCP-style
# listing and serves one call, and reports through its exit code, so it runs
# under ctest when the tests are built.
add_executable(scrydantic_standalone_tools_example standalone_tools.cpp)
target_link_libraries(
  scrydantic_standalone_tools_example
  PRIVATE scrydantic::scrydantic scrydantic_project_options
)
if(SCRYDANTIC_BUILD_TESTS)
  add_test(
    NAME scrydantic.example.standalone-tools
    COMMAND scrydantic_standalone_tools_example
  )
endif()
