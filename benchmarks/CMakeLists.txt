add_library(
  scry_benchmark_scenarios
  STATIC
    admission_scenarios.cpp
    ownership_scenarios.cpp
    runtime_scenarios.cpp
    scenarios.cpp
    tool_registry_scenarios.cpp
)
target_compile_features(scry_benchmark_scenarios PRIVATE cxx_std_23)
target_include_directories(
  scry_benchmark_scenarios
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
  PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(
  scry_benchmark_scenarios
  PUBLIC scry::scry
  PRIVATE scry_project_options
)

function(scry_add_benchmark_executable TARGET_NAME SOURCE_FILE MODE_NAME)
  add_executable(
    "${TARGET_NAME}"
    benchmark_main.cpp
    "${SOURCE_FILE}"
  )
  target_compile_features("${TARGET_NAME}" PRIVATE cxx_std_23)
  target_compile_definitions(
    "${TARGET_NAME}"
    PRIVATE
      "SCRY_BENCHMARK_MODE=\"${MODE_NAME}\""
      "SCRY_BENCHMARK_TARGET=\"${TARGET_NAME}\""
  )
  target_include_directories(
    "${TARGET_NAME}"
    PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}"
      "${PROJECT_SOURCE_DIR}/src"
  )
  target_link_libraries(
    "${TARGET_NAME}"
    PRIVATE
      benchmark::benchmark
      scry_benchmark_scenarios
      scry_project_options
  )
endfunction()

scry_add_benchmark_executable(
  scry_timing_benchmarks
  timing_benchmarks.cpp
  timing
)

scry_add_benchmark_executable(
  scry_allocation_benchmarks
  allocation_benchmarks.cpp
  allocation_pressure
)
target_sources(
  scry_allocation_benchmarks
  PRIVATE
    allocation_tracker.cpp
)
target_compile_definitions(
  scry_allocation_benchmarks
  PRIVATE SCRY_VALIDATE_ALLOCATION_TRACKER=1
)
