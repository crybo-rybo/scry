include(ScryFuzz)

scry_add_fuzzer(
  scry_sse_fuzz
  sse
  TEST_PREFIX "protocol."
  SOURCES ../protocol/sse_fuzz.cpp "${PROJECT_SOURCE_DIR}/src/protocol/sse.cpp"
)

set(
  scry_provider_fuzz_sources
  "${PROJECT_SOURCE_DIR}/src/core/error.cpp"
  "${PROJECT_SOURCE_DIR}/src/core/json_codec.cpp"
  "${PROJECT_SOURCE_DIR}/src/provider/anthropic_content.cpp"
  "${PROJECT_SOURCE_DIR}/src/provider/anthropic_request.cpp"
  "${PROJECT_SOURCE_DIR}/src/provider/anthropic_stream.cpp"
  "${PROJECT_SOURCE_DIR}/src/provider/openai_request.cpp"
  "${PROJECT_SOURCE_DIR}/src/provider/openai_stream.cpp"
  "${PROJECT_SOURCE_DIR}/src/provider/provider.cpp"
)
scry_add_fuzzer(
  scry_anthropic_fuzz
  anthropic
  TEST_PREFIX "provider."
  SOURCES ../provider/anthropic_fuzz.cpp ${scry_provider_fuzz_sources}
)
scry_add_fuzzer(
  scry_openai_fuzz
  openai
  TEST_PREFIX "provider."
  SOURCES ../provider/openai_fuzz.cpp ${scry_provider_fuzz_sources}
)

scry_add_fuzzer(
  scry_response_policy_fuzz
  response_policy
  TEST_PREFIX "transport."
  SOURCES
    ../transport/response_policy_fuzz.cpp
    "${PROJECT_SOURCE_DIR}/src/core/error.cpp"
    "${PROJECT_SOURCE_DIR}/src/core/json_codec.cpp"
    "${PROJECT_SOURCE_DIR}/src/transport/transport_policy.cpp"
)

scry_add_fuzzer(
  scry_conversation_fuzz
  conversation
  TEST_PREFIX "runtime."
  SOURCES ../runtime/conversation_fuzz.cpp
  LINK_LIBRARIES scry::scry
)
