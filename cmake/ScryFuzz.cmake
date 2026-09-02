# Registration for libFuzzer targets. Every fuzz target is built only under
# SCRY_BUILD_FUZZERS and is registered as a ctest test that replays its
# checked-in seed corpus. SCRY_FUZZ_RUNS controls the budget: the default of 0
# makes libFuzzer execute the seed corpus once and exit, which is a
# deterministic per-commit replay. The scheduled fuzz ring raises it.

set(
  SCRY_FUZZ_RUNS
  "0"
  CACHE STRING
  "libFuzzer -runs budget for every registered fuzz target; 0 replays the seed corpus once"
)

# scry_add_fuzzer(<target> <corpus> SOURCES <source>... [LINK_LIBRARIES <lib>...])
function(scry_add_fuzzer target corpus)
  cmake_parse_arguments(SCRY_FUZZER "" "" "SOURCES;LINK_LIBRARIES" ${ARGN})
  if(NOT SCRY_FUZZER_SOURCES)
    message(FATAL_ERROR "scry_add_fuzzer(${target}) requires SOURCES")
  endif()

  add_executable("${target}" ${SCRY_FUZZER_SOURCES})
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
  target_link_libraries(
    "${target}"
    PRIVATE scry_project_options ${SCRY_FUZZER_LINK_LIBRARIES}
  )

  file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/fuzz-corpus/${corpus}")
  add_test(
    NAME "${SCRY_FUZZ_TEST_PREFIX}${corpus}-fuzz"
    COMMAND
      "${target}"
      "-runs=${SCRY_FUZZ_RUNS}"
      -timeout=5
      -max_total_time=30
      "${PROJECT_BINARY_DIR}/fuzz-corpus/${corpus}"
      "${PROJECT_SOURCE_DIR}/tests/fuzz/corpus/${corpus}"
  )
  set_tests_properties(
    "${SCRY_FUZZ_TEST_PREFIX}${corpus}-fuzz"
    PROPERTIES TIMEOUT 45
  )
endfunction()

function(scry_require_fuzzer_compiler)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "SCRY_BUILD_FUZZERS requires a Clang-family compiler")
  endif()
endfunction()
