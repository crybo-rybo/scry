add_executable(
  scry_provider_tests
  coverage_tests.cpp
  openai_request_tests.cpp
  openai_stream_tests.cpp
  provider_tests.cpp
  stream_tests.cpp
  tool_boundary_tests.cpp
  wire_json_coverage_tests.cpp
)
target_include_directories(
  scry_provider_tests
  PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_include_directories(
  scry_provider_tests
  SYSTEM PRIVATE "${glaze_SOURCE_DIR}/include"
)
target_compile_definitions(
  scry_provider_tests
  PRIVATE
    "SCRY_ANTHROPIC_FIXTURE_DIR=\"${PROJECT_SOURCE_DIR}/tests/fixtures/anthropic\""
)
target_link_libraries(
  scry_provider_tests
  PRIVATE Catch2::Catch2WithMain scry::scry scry_project_options
)
catch_discover_tests(scry_provider_tests TEST_PREFIX "provider.")

if(SCRY_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "SCRY_BUILD_FUZZERS requires a Clang-family compiler")
  endif()
  set(
    scry_provider_fuzz_sources
    "${PROJECT_SOURCE_DIR}/src/provider/anthropic_content.cpp"
    "${PROJECT_SOURCE_DIR}/src/provider/anthropic_request.cpp"
    "${PROJECT_SOURCE_DIR}/src/provider/anthropic_stream.cpp"
    "${PROJECT_SOURCE_DIR}/src/provider/openai_content.cpp"
    "${PROJECT_SOURCE_DIR}/src/provider/openai_request.cpp"
    "${PROJECT_SOURCE_DIR}/src/provider/openai_stream.cpp"
    "${PROJECT_SOURCE_DIR}/src/provider/provider.cpp"
    "${PROJECT_SOURCE_DIR}/src/provider/wire_json.cpp"
  )

  function(scry_add_provider_fuzzer target source corpus)
    add_executable("${target}" "${source}" ${scry_provider_fuzz_sources})
    target_compile_features("${target}" PRIVATE cxx_std_23)
    target_include_directories(
      "${target}"
      PRIVATE "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src"
    )
    target_include_directories(
      "${target}"
      SYSTEM PRIVATE "${glaze_SOURCE_DIR}/include"
    )
    target_compile_options("${target}" PRIVATE -fsanitize=fuzzer)
    target_link_options("${target}" PRIVATE -fsanitize=fuzzer)
    target_link_libraries("${target}" PRIVATE scry_project_options)
    file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/fuzz-corpus/${corpus}")
    add_test(
      NAME "provider.${corpus}-fuzz"
      COMMAND
        "${target}"
        -runs=1000
        -timeout=5
        -max_total_time=30
        "${PROJECT_BINARY_DIR}/fuzz-corpus/${corpus}"
        "${PROJECT_SOURCE_DIR}/tests/fuzz/corpus/${corpus}"
    )
    set_tests_properties("provider.${corpus}-fuzz" PROPERTIES TIMEOUT 45)
  endfunction()

  scry_add_provider_fuzzer(
    scry_anthropic_fuzz
    anthropic_fuzz.cpp
    anthropic
  )
  scry_add_provider_fuzzer(
    scry_openai_fuzz
    openai_fuzz.cpp
    openai
  )
endif()
