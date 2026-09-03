if(SCRY_BUILD_FUZZERS)
  scry_require_fuzzer_compiler()
  set(SCRY_FUZZ_TEST_PREFIX "transport.")
  scry_add_fuzzer(
    scry_response_policy_fuzz
    response_policy
    SOURCES
      response_policy_fuzz.cpp
      "${PROJECT_SOURCE_DIR}/src/core/error.cpp"
      "${PROJECT_SOURCE_DIR}/src/core/json_codec.cpp"
      "${PROJECT_SOURCE_DIR}/src/transport/transport_policy.cpp"
  )
endif()

# SCRY_CLANG_TOOLING contributes only the fuzz targets from this directory; the
# Catch2 suite below reaches the C++26 public API and cannot compile under Clang.
if(SCRY_CLANG_TOOLING)
  return()
endif()

add_executable(
  scry_transport_tests
  curl_transport_tests.cpp
  fake_transport_tests.cpp
  transport_policy_tests.cpp
  "${PROJECT_SOURCE_DIR}/tests/support/transport/loopback_server.cpp"
)
target_include_directories(
  scry_transport_tests
  PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
)
target_link_libraries(
  scry_transport_tests
  PRIVATE
    Catch2::Catch2WithMain
    scry::scry
    scry_project_options
)
catch_discover_tests(
  scry_transport_tests
  TEST_PREFIX "transport."
  PROPERTIES TIMEOUT 15
)
