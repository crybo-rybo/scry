#include "core/json_codec.hpp"
#include "core/model.hpp"
#include "fixture_support.hpp"
#include "provider/anthropic.hpp"
#include "provider/anthropic_content.hpp"
#include "provider/shared.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <scry/config.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
namespace {
using namespace scry;
using namespace scry::detail;
using namespace scry::test_fixtures;
[[nodiscard]] ProviderDecodeState text_state(AnthropicAdapter& adapter) {
  ProviderDecodeState state;
  start_anthropic_message(adapter, state);
  auto result = decode(
      adapter, "content_block_start",
      R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})",
      state);
  REQUIRE(result);
  return state;
}
[[nodiscard]] ProviderDecodeState tool_state(AnthropicAdapter& adapter) {
  ProviderDecodeState state;
  start_anthropic_message(adapter, state);
  auto result = decode(
      adapter, "content_block_start",
      R"({"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"tool_1","name":"lookup","input":{}}})",
      state);
  REQUIRE(result);
  return state;
}
} // namespace
TEST_CASE("provider error tokens remain bounded and safe") {
  CHECK_FALSE(sanitize_error_token(""));
  CHECK_FALSE(sanitize_error_token(std::string(97, 'a')));
  CHECK(sanitize_error_token("Safe_123") == "Safe_123");
  CHECK_FALSE(sanitize_error_token("unsafe-value"));
}
TEST_CASE("Anthropic content decoding covers text, tool, and rejection shapes") {
  auto text = decode_anthropic_content(
      json_value("{\"type\":\"text\",\"text\":\"answer\"}"), false);
  REQUIRE(text);
  CHECK(std::get<TextBlock>(*text).text == "answer");
  auto streamed_tool = decode_anthropic_content(
      json_value(
          "{\"type\":\"tool_use\",\"id\":\"id\",\"name\":\"lookup\",\"input\":{}}"),
      true);
  REQUIRE(streamed_tool);
  CHECK(std::get<ToolCallBlock>(*streamed_tool).arguments.text.empty());
  auto tool = decode_anthropic_content(
      json_value("{\"type\":\"tool_use\",\"id\":\"id\",\"name\":\"lookup\","
                 "\"input\":{\"x\":1}}"),
      false);
  REQUIRE(tool);
  CHECK(std::get<ToolCallBlock>(*tool).arguments.text == "{\"x\":1}");
  constexpr std::array invalid{
      "{}",
      "{\"type\":1}",
      "{\"type\":\"future\"}",
      "{\"type\":\"text\"}",
      "{\"type\":\"text\",\"text\":1}",
      "{\"type\":\"tool_use\",\"name\":\"lookup\",\"input\":{}}",
      "{\"type\":\"tool_use\",\"id\":\"id\",\"input\":{}}",
      "{\"type\":\"tool_use\",\"id\":\"id\",\"name\":\"lookup\"}",
  };
  for (const auto json : invalid) {
    const auto result = decode_anthropic_content(json_value(json), false);
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::protocol);
  }
}
TEST_CASE("Anthropic finish and usage decoding covers every wire variant") {
  CHECK(decode_anthropic_finish(std::nullopt) == FinishReason::unknown);
  for (const auto reason : {"end_turn", "stop_sequence"}) {
    CHECK(decode_anthropic_finish(reason) == FinishReason::completed);
  }
  CHECK(decode_anthropic_finish("max_tokens") == FinishReason::length);
  CHECK(decode_anthropic_finish("tool_use") == FinishReason::tool_use);
  CHECK(decode_anthropic_finish("future") == FinishReason::unknown);
  Usage usage{.input_tokens = 1, .output_tokens = 2};
  REQUIRE(apply_anthropic_usage(json_value("{}"), usage));
  REQUIRE_FALSE(apply_anthropic_usage(json_value(R"({"usage":[]})"), usage));
  REQUIRE(apply_anthropic_usage(
      json_value(R"({"usage":{"input_tokens":7,"output_tokens":9}})"), usage));
  CHECK(usage.input_tokens == 7);
  REQUIRE(
      apply_anthropic_usage(json_value(R"({"usage":{"input_tokens":null}})"), usage));
  REQUIRE_FALSE(
      apply_anthropic_usage(json_value(R"({"usage":{"input_tokens":"7"}})"), usage));
  REQUIRE_FALSE(
      apply_anthropic_usage(json_value(R"({"usage":{"output_tokens":"9"}})"), usage));
}
TEST_CASE("Anthropic endpoint normalization appends the Messages path once") {
  AnthropicAdapter adapter;
  constexpr std::array cases{
      std::pair{"https://api.anthropic.test", "https://api.anthropic.test/v1/messages"},
      std::pair{"https://api.anthropic.test/v1/messages///",
                "https://api.anthropic.test/v1/messages"},
  };
  const ModelRequest request{
      .messages = {Message{.role = Role::user, .content = {TextBlock{.text = "hi"}}}},
      .sampling = SamplingConfig{.max_tokens = 32},
  };
  for (const auto& [base, expected] : cases) {
    INFO(base);
    auto config = anthropic_config();
    config.base_url = base;
    const auto encoded = adapter.make_request(config, request);
    REQUIRE(encoded);
    CHECK(encoded->url == expected);
  }
}
TEST_CASE("Anthropic stream start handles initial content and rejects bad envelopes") {
  AnthropicAdapter adapter;
  ProviderDecodeState state;
  state.response.provider_request_id = "header-id";
  auto result = decode(
      adapter, "message_start",
      R"({"type":"message_start","message":{"type":"message","request_id":"body-id","content":[{"type":"text","text":"initial"},{"type":"tool_use","id":"id","name":"lookup","input":{}}],"stop_reason":null,"usage":{"input_tokens":2}}})",
      state);
  REQUIRE(result);
  REQUIRE(result->size() == 1);
  CHECK(std::get<ProviderTextDelta>(result->front()).text == "initial");
  CHECK(state.response.provider_request_id == "header-id");
  require_protocol(decode(
      adapter, "message_start",
      R"({"type":"message_start","message":{"type":"message","content":[]}})", state));
  constexpr std::array invalid{
      R"({"type":"message_start"})",
      R"({"type":"message_start","message":[]})",
      R"({"type":"message_start","message":{"content":[]}})",
      R"({"type":"message_start","message":{"type":"future","content":[]}})",
      R"({"type":"message_start","message":{"type":"message","usage":[],"content":[]}})",
      R"({"type":"message_start","message":{"type":"message","stop_reason":7,"content":[]}})",
      R"({"type":"message_start","message":{"type":"message"}})",
      R"({"type":"message_start","message":{"type":"message","content":{}}})",
      R"({"type":"message_start","message":{"type":"message","content":[{"type":"future"}]}})",
  };
  for (const auto body : invalid) {
    ProviderDecodeState fresh;
    require_protocol(decode(adapter, "message_start", body, fresh));
  }
}
TEST_CASE("Anthropic stream content start rejects invalid lifecycle and shapes") {
  AnthropicAdapter adapter;
  ProviderDecodeState empty;
  require_protocol(decode(
      adapter, "content_block_start",
      R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})",
      empty));
  constexpr std::array invalid{
      R"({"type":"content_block_start","content_block":{"type":"text","text":""}})",
      R"({"type":"content_block_start","index":"0","content_block":{"type":"text","text":""}})",
      R"({"type":"content_block_start","index":1,"content_block":{"type":"text","text":""}})",
      R"({"type":"content_block_start","index":0})",
      R"({"type":"content_block_start","index":0,"content_block":[]})",
      R"({"type":"content_block_start","index":0,"content_block":{"type":"future"}})",
  };
  for (const auto body : invalid) {
    ProviderDecodeState state;
    start_anthropic_message(adapter, state);
    require_protocol(decode(adapter, "content_block_start", body, state));
  }
  auto state = text_state(adapter);
  require_protocol(decode(
      adapter, "content_block_start",
      R"({"type":"content_block_start","index":1,"content_block":{"type":"text","text":""}})",
      state));
  state = ProviderDecodeState{};
  start_anthropic_message(adapter, state);
  std::get<AnthropicProviderDecodeState>(state.dialect).finish_observed = true;
  require_protocol(decode(
      adapter, "content_block_start",
      R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})",
      state));
}
TEST_CASE("Anthropic stream content deltas validate indexes, targets, and payloads") {
  AnthropicAdapter adapter;
  constexpr std::array invalid_text{
      R"({"type":"content_block_delta","index":1,"delta":{"type":"text_delta","text":"x"}})",
      R"({"type":"content_block_delta","index":0})",
      R"({"type":"content_block_delta","index":0,"delta":[]})",
      R"({"type":"content_block_delta","index":0,"delta":{}})",
      R"({"type":"content_block_delta","index":0,"delta":{"type":"future"}})",
      R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta"}})",
      R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":7}})",
      R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{}"}})",
  };
  for (const auto body : invalid_text) {
    auto state = text_state(adapter);
    require_protocol(decode(adapter, "content_block_delta", body, state));
  }
  auto text = text_state(adapter);
  auto delta = decode(
      adapter, "content_block_delta",
      R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"x"}})",
      text);
  REQUIRE(delta);
  CHECK(std::get<ProviderTextDelta>(delta->front()).text == "x");
  auto tool = tool_state(adapter);
  require_protocol(decode(
      adapter, "content_block_delta",
      R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"x"}})",
      tool));
  for (
      const auto body :
      {R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta"}})",
       R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":7}})"}) {
    tool = tool_state(adapter);
    require_protocol(decode(adapter, "content_block_delta", body, tool));
  }
}
TEST_CASE("Anthropic stream content stop canonicalizes tools and closes text") {
  AnthropicAdapter adapter;
  auto text = text_state(adapter);
  REQUIRE(decode(adapter, "content_block_stop",
                 R"({"type":"content_block_stop","index":0})", text));
  CHECK_FALSE(
      std::get<AnthropicProviderDecodeState>(text.dialect).active_content_index);
  auto tool = tool_state(adapter);
  REQUIRE(decode(adapter, "content_block_stop",
                 R"({"type":"content_block_stop","index":0})", tool));
  CHECK(std::get<ToolCallBlock>(tool.response.content.front()).arguments.text == "{}");
  tool = tool_state(adapter);
  REQUIRE(decode(
      adapter, "content_block_delta",
      R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{"}})",
      tool));
  require_protocol(decode(adapter, "content_block_stop",
                          R"({"type":"content_block_stop","index":0})", tool));
}
TEST_CASE("Anthropic stream message finish enforces lifecycle and usage") {
  AnthropicAdapter adapter;
  ProviderDecodeState empty;
  require_protocol(
      decode(adapter, "message_delta",
             R"({"type":"message_delta","delta":{"stop_reason":"end_turn"}})", empty));
  constexpr std::array invalid{
      R"({"type":"message_delta"})",
      R"({"type":"message_delta","delta":[]})",
      R"({"type":"message_delta","delta":{"stop_reason":7}})",
      R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":[]})",
  };
  for (const auto body : invalid) {
    ProviderDecodeState state;
    start_anthropic_message(adapter, state);
    require_protocol(decode(adapter, "message_delta", body, state));
  }
  ProviderDecodeState state;
  start_anthropic_message(adapter, state);
  REQUIRE(decode(adapter, "message_delta",
                 R"({"type":"message_delta","delta":{"stop_reason":null}})", state));
  require_protocol(
      decode(adapter, "message_stop", R"({"type":"message_stop"})", state));
  state = text_state(adapter);
  require_protocol(
      decode(adapter, "message_stop", R"({"type":"message_stop"})", state));
  state = ProviderDecodeState{};
  start_anthropic_message(adapter, state);
  REQUIRE(decode(
      adapter, "message_delta",
      R"({"type":"message_delta","delta":{"stop_reason":"max_tokens"},"usage":{"output_tokens":3}})",
      state));
  REQUIRE(decode(adapter, "message_stop", R"({"type":"message_stop"})", state));
  CHECK(state.response.finish_reason == FinishReason::length);
  CHECK(state.response.usage.output_tokens == 3);
}
TEST_CASE("Anthropic stream envelopes and provider errors cover safe categories") {
  AnthropicAdapter adapter;
  ProviderDecodeState state;
  require_protocol(decode(adapter, "ping", R"({})", state));
  require_protocol(decode(adapter, "ping", R"({"type":"message_stop"})", state));
  auto ignored = decode(adapter, "message", R"({"type":"future"})", state);
  REQUIRE(ignored);
  CHECK(ignored->empty());
  constexpr std::array cases{
      std::pair{"authentication_error", ErrorCategory::authentication},
      std::pair{"permission_error", ErrorCategory::authentication},
      std::pair{"rate_limit_error", ErrorCategory::rate_limit},
      std::pair{"overloaded_error", ErrorCategory::network},
      std::pair{"api_error", ErrorCategory::network},
      std::pair{"other_error", ErrorCategory::protocol},
  };
  for (const auto& [type, category] : cases) {
    const auto body = std::string{R"({"type":"error","error":{"type":")"} + type +
                      R"(","message":"private"},"request_id":"id"})";
    auto result = decode(adapter, "error", body, state);
    REQUIRE_FALSE(result);
    CHECK(result.error().category == category);
    CHECK(result.error().provider_detail == std::string{"anthropic:"} + type);
    CHECK(result.error().message.find("private") == std::string::npos);
    CHECK(result.error().provider_request_id == "id");
    CHECK(result.error().retryable == (category == ErrorCategory::rate_limit ||
                                       category == ErrorCategory::network));
  }
  for (const auto body :
       {R"({"type":"error"})", R"({"type":"error","error":[]})",
        R"({"type":"error","error":{"type":7}})",
        R"({"type":"error","error":{"type":"unsafe-secret-value"}})"}) {
    auto result = decode(adapter, "error", body, state);
    REQUIRE_FALSE(result);
    CHECK(result.error().provider_detail == "anthropic:unknown_error");
  }
}
