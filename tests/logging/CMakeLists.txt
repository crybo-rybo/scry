add_executable(scry_logging_probe logging_probe.cpp)
target_include_directories(
  scry_logging_probe
  PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_compile_definitions(scry_logging_probe PRIVATE SCRY_ENABLE_LOGGING=1)
target_link_libraries(
  scry_logging_probe
  PRIVATE scry::scry scry_project_options
)

foreach(SCRY_LOGGING_MODE IN ITEMS unset empty explicit)
  add_test(
    NAME "logging.${SCRY_LOGGING_MODE} destination behavior"
    COMMAND
      "${CMAKE_COMMAND}"
      "-DPROBE=$<TARGET_FILE:scry_logging_probe>"
      "-DMODE=${SCRY_LOGGING_MODE}"
      "-DWORK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/${SCRY_LOGGING_MODE}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/verify_logging.cmake"
  )
endforeach()
