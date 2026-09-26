# Registration for libFuzzer targets. Every fuzz target is built only under
# SCRY_BUILD_FUZZERS and is registered as a ctest test that replays its
# checked-in seed corpus: -runs=0 makes libFuzzer execute the corpus once and
# exit, which is a deterministic per-commit replay. The scheduled long searches
# run the binaries directly (scripts/ci-nightly-fuzz.sh).

# scry_add_fuzzer(<target> <corpus> SOURCES <source>...
#                 [TEST_PREFIX <prefix>] [LINK_LIBRARIES <lib>...])
function(scry_add_fuzzer target corpus)
  cmake_parse_arguments(SCRY_FUZZER "" "TEST_PREFIX" "SOURCES;LINK_LIBRARIES" ${ARGN})

  add_executable("${target}" ${SCRY_FUZZER_SOURCES})
  target_compile_features("${target}" PRIVATE cxx_std_23)
  target_include_directories(
    "${target}"
    PRIVATE "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src"
  )
  target_include_directories(
    "${target}"
    SYSTEM PRIVATE "${SCRY_GLAZE_INCLUDE_DIR}"
  )
  target_compile_options("${target}" PRIVATE -fsanitize=fuzzer)
  target_link_options("${target}" PRIVATE -fsanitize=fuzzer)
  target_link_libraries(
    "${target}"
    PRIVATE scry_project_options ${SCRY_FUZZER_LINK_LIBRARIES}
  )

  file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/fuzz-corpus/${corpus}")
  add_test(
    NAME "${SCRY_FUZZER_TEST_PREFIX}${corpus}-fuzz"
    COMMAND
      "${target}"
      -runs=0
      -timeout=5
      -max_total_time=30
      "${PROJECT_BINARY_DIR}/fuzz-corpus/${corpus}"
      "${PROJECT_SOURCE_DIR}/tests/fuzz/corpus/${corpus}"
  )
  set_tests_properties(
    "${SCRY_FUZZER_TEST_PREFIX}${corpus}-fuzz"
    PROPERTIES TIMEOUT 45
  )
endfunction()
