add_executable(
  scry_showcase_tests
  chat_panel_tests.cpp
  npc_dispatch_tests.cpp
  npc_world_tests.cpp
  "${PROJECT_SOURCE_DIR}/examples/npc/world.cpp"
)
target_include_directories(
  scry_showcase_tests
  PRIVATE
    "${PROJECT_SOURCE_DIR}/examples/imgui"
    "${PROJECT_SOURCE_DIR}/examples/npc"
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
)
target_link_libraries(
  scry_showcase_tests
  PRIVATE
    Catch2::Catch2WithMain
    scry_imgui_chat_panel
    scry_project_options
)
catch_discover_tests(
  scry_showcase_tests
  TEST_PREFIX "showcase."
  PROPERTIES LABELS showcase
)

add_executable(scry_imgui_headless_smoke imgui_headless_smoke.cpp)
target_include_directories(
  scry_imgui_headless_smoke
  PRIVATE "${PROJECT_SOURCE_DIR}/examples/imgui"
)
target_link_libraries(
  scry_imgui_headless_smoke
  PRIVATE
    scry_imgui_chat_panel
    scry_imgui_vendor
    scry_project_options
)
add_test(NAME showcase.imgui-headless-frame COMMAND scry_imgui_headless_smoke)
set_tests_properties(
  showcase.imgui-headless-frame
  PROPERTIES LABELS showcase
)
