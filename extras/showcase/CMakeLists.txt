# Standalone project for Scry's opt-in showcase: a Dear ImGui chat panel and a
# grid-NPC example. Deliberately outside the root build — the library is the
# deliverable, and nothing here is built or installed by it. Configure this
# directory directly:
#
#   cmake -S extras/showcase -B build/showcase -G Ninja -DCMAKE_BUILD_TYPE=Debug
#
# ./scripts/ci-showcase.sh builds this project; the core CI script audits the
# installed package.

cmake_minimum_required(VERSION 3.28)

project(scry_showcase LANGUAGES CXX)

# No C++ modules here either; see the root CMakeLists.txt for why scanning
# must be off for GCC on macOS.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

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

# Dear ImGui v1.92.9b is MIT-licensed and intentionally build-only.
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG f1cc2ae15e53a861a874c3034aae6798fde194ab
  SYSTEM
)
FetchContent_MakeAvailable(imgui)

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
