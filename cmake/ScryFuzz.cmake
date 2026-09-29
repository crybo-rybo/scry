# Registration for fuzz targets. A fuzz target is one LLVMFuzzerTestOneInput
# entry point over a checked-in seed corpus under tests/fuzz/corpus/<corpus>, and
# is built one of two ways, by which side of the kernel boundary it exercises.
#
# scry_add_fuzzer: a kernel target, built as a libFuzzer binary only under
# SCRY_BUILD_FUZZERS (the Clang tooling build). Its ctest test replays the seed
# corpus: -runs=0 makes libFuzzer execute the corpus once and exit, which is a
# deterministic per-commit replay.
#
# scry_add_fuzz_replay: a target over the reflective side of the library, which
# links scry and so builds only with GCC. It links tests/fuzz/replay_main.cpp in
# place of libFuzzer and is registered in the ordinary test build, where it
# replays the seed corpus once; under the asan preset that replay runs with ASan
# and UBSan. There is no coverage-guided search for these targets.

# scry_add_fuzzer(<target> <corpus> SOURCES <source>...
#                 [TEST_PREFIX <prefix>] [TEST_NAME <name>]
#                 [SEED_CORPORA <corpus>...] [LINK_LIBRARIES <lib>...])
#
# The test is named <prefix><corpus>-fuzz unless TEST_NAME replaces the part
# after the prefix. SEED_CORPORA names further checked-in corpora the replay runs
# as well, for a target that reads the other targets' inputs too.
function(scry_add_fuzzer target corpus)
  cmake_parse_arguments(
    SCRY_FUZZER
    ""
    "TEST_PREFIX;TEST_NAME"
    "SOURCES;LINK_LIBRARIES;SEED_CORPORA"
    ${ARGN}
  )
  if(NOT SCRY_FUZZER_TEST_NAME)
    set(SCRY_FUZZER_TEST_NAME "${corpus}-fuzz")
  endif()
  set(SCRY_FUZZER_SEEDS "${PROJECT_SOURCE_DIR}/tests/fuzz/corpus/${corpus}")
  foreach(SCRY_FUZZER_SEED IN LISTS SCRY_FUZZER_SEED_CORPORA)
    list(APPEND SCRY_FUZZER_SEEDS "${PROJECT_SOURCE_DIR}/tests/fuzz/corpus/${SCRY_FUZZER_SEED}")
  endforeach()

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
    NAME "${SCRY_FUZZER_TEST_PREFIX}${SCRY_FUZZER_TEST_NAME}"
    COMMAND
      "${target}"
      -runs=0
      -timeout=5
      -max_total_time=30
      "${PROJECT_BINARY_DIR}/fuzz-corpus/${corpus}"
      ${SCRY_FUZZER_SEEDS}
  )
  set_tests_properties(
    "${SCRY_FUZZER_TEST_PREFIX}${SCRY_FUZZER_TEST_NAME}"
    PROPERTIES TIMEOUT 45
  )
endfunction()

# scry_add_fuzz_replay(<target> <corpus> SOURCES <source>...
#                      [TEST_PREFIX <prefix>] [LINK_LIBRARIES <lib>...])
function(scry_add_fuzz_replay target corpus)
  cmake_parse_arguments(SCRY_REPLAY "" "TEST_PREFIX" "SOURCES;LINK_LIBRARIES" ${ARGN})

  add_executable(
    "${target}"
    "${PROJECT_SOURCE_DIR}/tests/fuzz/replay_main.cpp"
    ${SCRY_REPLAY_SOURCES}
  )
  target_compile_features("${target}" PRIVATE cxx_std_23)
  target_include_directories(
    "${target}"
    PRIVATE "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src"
  )
  target_link_libraries(
    "${target}"
    PRIVATE scry_project_options ${SCRY_REPLAY_LINK_LIBRARIES}
  )

  add_test(
    NAME "${SCRY_REPLAY_TEST_PREFIX}${corpus}-fuzz-replay"
    COMMAND "${target}" "${PROJECT_SOURCE_DIR}/tests/fuzz/corpus/${corpus}"
  )
  set_tests_properties(
    "${SCRY_REPLAY_TEST_PREFIX}${corpus}-fuzz-replay"
    PROPERTIES TIMEOUT 45
  )
endfunction()
