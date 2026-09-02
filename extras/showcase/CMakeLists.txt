# Standalone project for Scry's opt-in showcase: a Dear ImGui chat panel and a
# grid-NPC example, with their tests. Deliberately outside the root build — the
# library is the deliverable, and nothing here is built, tested, or installed by
# it. Configure this directory directly:
#
#   cmake -S extras/showcase -B build/showcase -G Ninja -DCMAKE_BUILD_TYPE=Debug
#
# ./scripts/ci-showcase.sh does exactly that and audits that no showcase artifact
# reaches the installed package.

cmake_minimum_required(VERSION 3.28)

project(scry_showcase LANGUAGES CXX)

include(CTest)
include(FetchContent)

get_filename_component(
  SCRY_REPOSITORY_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/../.."
  ABSOLUTE
)

# Build the library itself, without its own tests or example: this project is a
# consumer of scry::scry that happens to live in the same repository.
set(SCRY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SCRY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SCRY_WARNINGS_AS_ERRORS ON CACHE BOOL "" FORCE)
add_subdirectory("${SCRY_REPOSITORY_ROOT}" scry)

# Dear ImGui v1.92.8 is MIT-licensed and intentionally build-only.
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG 8936b58fe26e8c3da834b8f60b06511d537b4c63
  SYSTEM
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG dfc2dff8d70d083c60c1c6986030e5389a867a93
  SYSTEM
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(Catch2)
foreach(SCRY_CATCH_TARGET IN ITEMS Catch2 Catch2WithMain)
  target_compile_features("${SCRY_CATCH_TARGET}" PRIVATE cxx_std_23)
endforeach()
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

add_library(
  scry_imgui_vendor
  STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
)
target_include_directories(
  scry_imgui_vendor
  SYSTEM PUBLIC "${imgui_SOURCE_DIR}"
)

add_library(scry_imgui_chat_panel STATIC imgui/chat_panel.cpp)
target_compile_features(scry_imgui_chat_panel PRIVATE cxx_std_23)
target_include_directories(
  scry_imgui_chat_panel
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/imgui"
)
target_link_libraries(
  scry_imgui_chat_panel
  PUBLIC scry::scry
  PRIVATE scry_imgui_vendor scry_project_options
)

add_executable(
  scry_npc_showcase
  npc/main.cpp
  npc/world.cpp
)
target_include_directories(
  scry_npc_showcase
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/npc"
)
target_link_libraries(
  scry_npc_showcase
  PRIVATE scry::scry scry_project_options
)

add_executable(
  scry_showcase_tests
  tests/chat_panel_tests.cpp
  tests/npc_completion_tests.cpp
  tests/npc_dispatch_tests.cpp
  tests/npc_world_tests.cpp
  npc/world.cpp
)
# The one concession this project makes: its tests reach through the library's
# internal test seam (runtime/test_access.hpp, support/harness_test_support.hpp)
# so an NPC tool round can run against an injected transport.
target_include_directories(
  scry_showcase_tests
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/npc"
    "${SCRY_REPOSITORY_ROOT}/src"
    "${SCRY_REPOSITORY_ROOT}/tests"
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

add_executable(scry_imgui_headless_smoke tests/imgui_headless_smoke.cpp)
target_include_directories(
  scry_imgui_headless_smoke
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/imgui"
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
