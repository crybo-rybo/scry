if(SCRY_BUILD_EXAMPLES)
  add_executable(scry_canonical_example main_loop.cpp)
  target_link_libraries(
    scry_canonical_example
    PRIVATE scry::scry scry_project_options
  )

  if(SCRY_ENABLE_REFLECTION)
    add_executable(scry_reflection_example reflection_tools.cpp)
    target_link_libraries(
      scry_reflection_example
      PRIVATE scry::reflection scry_project_options
    )
  endif()
endif()

if(NOT SCRY_BUILD_IMGUI_SHOWCASE)
  return()
endif()

# Dear ImGui v1.92.8 is MIT-licensed and intentionally build-only.
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG 8936b58fe26e8c3da834b8f60b06511d537b4c63
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
