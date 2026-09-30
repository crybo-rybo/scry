# Tests of kernel components that link the scry_kernel objects alone, compiled
# as C++23 like the kernel itself.
add_executable(
  scry_kernel_json_tests
  json_document_tests.cpp
  json_fixture_tests.cpp
)
target_compile_features(scry_kernel_json_tests PRIVATE cxx_std_23)
target_include_directories(
  scry_kernel_json_tests
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
)
# The fixtures pin the canonical form; the corpora supply the prefixes the
# truncation replay walks.
target_compile_definitions(
  scry_kernel_json_tests
  PRIVATE
    "SCRY_JSON_FIXTURE_DIR=\"${SCRY_JSON_FIXTURE_DIR}\""
    "SCRY_FUZZ_CORPUS_DIR=\"${PROJECT_SOURCE_DIR}/tests/fuzz/corpus\""
)
target_link_libraries(
  scry_kernel_json_tests
  PRIVATE Catch2::Catch2WithMain scry_kernel scry_project_options
)
catch_discover_tests(scry_kernel_json_tests TEST_PREFIX "kernel.")
