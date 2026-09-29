# The kernel's libFuzzer targets, built only in the SCRY_CLANG_TOOLING build.
# Each links the scry_kernel objects, which carry SanitizerCoverage there. The
# reflective-side targets (anthropic, openai, conversation) link scry, which only
# GCC compiles; tests/CMakeLists.txt replays their corpora in the GCC build.
include(ScryFuzz)

scry_add_fuzzer(
  scry_sse_fuzz
  sse
  TEST_PREFIX "protocol."
  SOURCES ../protocol/sse_fuzz.cpp
  LINK_LIBRARIES scry_kernel
)

scry_add_fuzzer(
  scry_response_policy_fuzz
  response_policy
  TEST_PREFIX "transport."
  SOURCES ../transport/response_policy_fuzz.cpp
  LINK_LIBRARIES scry_kernel
)

scry_add_fuzzer(
  scry_json_fuzz
  json
  TEST_PREFIX "kernel."
  SOURCES json_fuzz.cpp
  LINK_LIBRARIES scry_kernel
)
