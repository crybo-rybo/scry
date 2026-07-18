#include "core/model.hpp"
#include "provider/anthropic.hpp"
#include "provider/anthropic_content.hpp"
#include "provider/anthropic_error.hpp"
#include "provider/wire_json.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
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
using StreamResult = Result<std::vector<ProviderEvent>>;
[[nodiscard]] StreamResult event(AnthropicAdapter& adapter, const std::string_view name,
                                 const std::string_view data,
                                 ProviderDecodeState& state) {
  return adapter.parse_stream_event(name, data, state);
}
[[nodiscard]] WireValue wire(const std::string_view text) {
  auto value = parse_wire_json(text, ErrorCategory::protocol, "invalid test JSON");
  REQUIRE(value);
  return std::move(*value);
}
[[nodiscard]] Config config() {
  return {
      .base_url = "https://api.anthropic.test",
      .api_key = "sanitized-key",
      .model = "fallback-model",
  };
}
[[nodiscard]] ModelRequest request() {
  return {
      .model = "request-model",
      .messages = {Message{
          .role = Role::user,
          .content = {TextBlock{.text = "hello"}},
      }},
      .sampling =
          SamplingConfig{
              .temperature = 0.5,
              .max_tokens = 32,
          },
  };
}
void require_protocol(StreamResult result) {
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::protocol);
}
void require_request_error(AnthropicAdapter& adapter, const Config& value,
                           const ModelRequest& model_request) {
  const auto result = adapter.make_request(value, model_request);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::invalid_config);
  CHECK(result.error().message.find("sanitized-key") == std::string::npos);
}
void start_message(AnthropicAdapter& adapter, ProviderDecodeState& state) {
  auto result = event(
      adapter, "message_start",
      R"({"type":"message_start","message":{"type":"message","content":[],"stop_reason":null}})",
      state);
  REQUIRE(result);
}
[[nodiscard]] ProviderDecodeState text_state(AnthropicAdapter& adapter) {
  ProviderDecodeState state;
  start_message(adapter, state);
  auto result = event(
      adapter, "content_block_start",
      R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})",
      state);
  REQUIRE(result);
  return state;
}
[[nodiscard]] ProviderDecodeState tool_state(AnthropicAdapter& adapter) {
  ProviderDecodeState state;
  start_message(adapter, state);
  auto result = event(
      adapter, "content_block_start",
      R"({"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"tool_1","name":"lookup","input":{}}})",
      state);
  REQUIRE(result);
  return state;
}
} // namespace
TEST_CASE("wire JSON accessors distinguish absence, null, type, and value") {
  const auto scalar = wire("7");
  CHECK(wire_field(scalar, "value") == nullptr);
  const auto object =
      wire(R"({"string":"value","array":[],"null":null,"uint":7,"wrong":false})");
  CHECK(wire_field(object, "missing") == nullptr);
  CHECK(std::string{*required_wire_string(object, "string")} == "value");
  for (const auto name : {"missing", "wrong"}) {
    const auto result = required_wire_string(object, name);
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::protocol);
  }
  REQUIRE(required_wire_array(object, "array"));
  for (const auto name : {"missing", "wrong"}) {
    REQUIRE_FALSE(required_wire_array(object, name));
  }
  CHECK_FALSE(*optional_wire_string(object, "missing"));
  CHECK_FALSE(*optional_wire_string(object, "null"));
  REQUIRE_FALSE(optional_wire_string(object, "wrong"));
  CHECK(std::string{**optional_wire_string(object, "string")} == "value");
  CHECK_FALSE(*optional_wire_uint(object, "missing"));
  CHECK_FALSE(*optional_wire_uint(object, "null"));
  REQUIRE_FALSE(optional_wire_uint(object, "wrong"));
  CHECK(**optional_wire_uint(object, "uint") == 7);
}
TEST_CASE("wire JSON errors and provider error types remain bounded and safe") {
  const auto malformed =
      parse_wire_json("{", ErrorCategory::invalid_config, "boundary failed");
  REQUIRE_FALSE(malformed);
  CHECK(malformed.error().category == ErrorCategory::invalid_config);
  const auto encoded =
      write_wire_json(wire(R"({"safe":true})"), ErrorCategory::protocol, "write");
  REQUIRE(encoded);
  CHECK(sanitize_anthropic_error_type("") == "unknown_error");
  CHECK(sanitize_anthropic_error_type(std::string(97, 'a')) == "unknown_error");
  CHECK(sanitize_anthropic_error_type("Safe_123") == "Safe_123");
  CHECK(sanitize_anthropic_error_type("unsafe-value") == "unknown_error");
  REQUIRE(make_provider_adapter(ProviderDialect::anthropic));
  REQUIRE_FALSE(make_provider_adapter(ProviderDialect::openai_compatible));
  const auto unknown =
      make_provider_adapter(static_cast<ProviderDialect>(std::uint8_t{255}));
  REQUIRE_FALSE(unknown);
}
TEST_CASE("Anthropic content decoding covers text, tool, and rejection shapes") {
  auto text =
      decode_anthropic_content(wire("{\"type\":\"text\",\"text\":\"answer\"}"), false);
  REQUIRE(text);
  CHECK(std::get<TextBlock>(*text).text == "answer");
  auto streamed_tool = decode_anthropic_content(
      wire("{\"type\":\"tool_use\",\"id\":\"id\",\"name\":\"lookup\",\"input\":{}}"),
      true);
  REQUIRE(streamed_tool);
  CHECK(std::get<ToolCallBlock>(*streamed_tool).arguments.text.empty());
  auto tool = decode_anthropic_content(
      wire("{\"type\":\"tool_use\",\"id\":\"id\",\"name\":\"lookup\","
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
    const auto result = decode_anthropic_content(wire(json), false);
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::protocol);
  }
}
TEST_CASE("Anthropic finish and usage decoding covers every wire variant") {
  CHECK(*decode_anthropic_finish(std::nullopt) == FinishReason::unknown);
  for (const auto reason : {"end_turn", "stop_sequence"}) {
    CHECK(*decode_anthropic_finish(reason) == FinishReason::completed);
  }
  CHECK(*decode_anthropic_finish("max_tokens") == FinishReason::length);
  CHECK(*decode_anthropic_finish("tool_use") == FinishReason::tool_use);
  CHECK(*decode_anthropic_finish("future") == FinishReason::unknown);
  Usage usage{.input_tokens = 1, .output_tokens = 2};
  REQUIRE(apply_anthropic_usage(wire("{}"), usage));
  REQUIRE_FALSE(apply_anthropic_usage(wire(R"({"usage":[]})"), usage));
  REQUIRE(apply_anthropic_usage(
      wire(R"({"usage":{"input_tokens":7,"output_tokens":9}})"), usage));
  CHECK(usage.input_tokens == 7);
  REQUIRE(apply_anthropic_usage(wire(R"({"usage":{"input_tokens":null}})"), usage));
  REQUIRE_FALSE(
      apply_anthropic_usage(wire(R"({"usage":{"input_tokens":"7"}})"), usage));
  REQUIRE_FALSE(
      apply_anthropic_usage(wire(R"({"usage":{"output_tokens":"9"}})"), usage));
}
TEST_CASE("Anthropic request validation rejects every invalid boundary") {
  AnthropicAdapter adapter;
  auto valid_config = config();
  auto valid_request = request();
  auto invalid_config = valid_config;
  invalid_config.base_url.clear();
  require_request_error(adapter, invalid_config, valid_request);
  invalid_config = valid_config;
  invalid_config.api_key.clear();
  require_request_error(adapter, invalid_config, valid_request);
  invalid_config.model.clear();
  auto invalid_request = valid_request;
  invalid_request.model.clear();
  require_request_error(adapter, invalid_config, invalid_request);
  for (const auto temperature : {std::numeric_limits<double>::quiet_NaN(), -0.1, 1.1}) {
    invalid_request = valid_request;
    invalid_request.sampling.temperature = temperature;
    require_request_error(adapter, valid_config, invalid_request);
  }
  for (const auto top_p : {std::numeric_limits<double>::quiet_NaN(), 0.0, -0.1, 1.1}) {
    invalid_request = valid_request;
    invalid_request.sampling.top_p = top_p;
    require_request_error(adapter, valid_config, invalid_request);
  }
  invalid_request = valid_request;
  invalid_request.sampling.max_tokens.reset();
  require_request_error(adapter, valid_config, invalid_request);
  invalid_request.sampling.max_tokens = 0;
  require_request_error(adapter, valid_config, invalid_request);
}
TEST_CASE("Anthropic request encoding preserves optional and tool branches") {
  AnthropicAdapter adapter;
  auto value_config = config();
  value_config.base_url = "https://api.anthropic.test/v1/messages///";
  value_config.tls_verify_peer = false;
  auto value_request = request();
  value_request.model.clear();
  value_request.system_prompt = "system";
  value_request.sampling.top_p = 0.8;
  value_request.streaming = true;
  value_request.tools.push_back({.name = "lookup",
                                 .description = "lookup",
                                 .input_schema = {.text = R"({"type":"object"})"}});
  value_request.messages.push_back(
      {.role = Role::assistant,
       .content = {ToolCallBlock{
           .id = "id", .name = "lookup", .arguments = {.text = R"({"x":1})"}}}});
  value_request.messages.push_back(
      {.role = Role::user,
       .content = {ToolResultBlock{.tool_call_id = "id",
                                   .result = {.text = R"({"answer":2})"},
                                   .is_error = true}}});
  const auto encoded = adapter.make_request(value_config, value_request);
  REQUIRE(encoded);
  CHECK(encoded->url == "https://api.anthropic.test/v1/messages");
  CHECK(encoded->body.find(R"("model":"fallback-model")") != std::string::npos);
  CHECK(encoded->body.find(R"("system":"system")") != std::string::npos);
  CHECK(encoded->body.find(R"("top_p":0.8)") != std::string::npos);
  CHECK(encoded->body.find(R"("is_error":true)") != std::string::npos);
  CHECK(encoded->body.find(R"("tools")") != std::string::npos);
}
TEST_CASE("Anthropic request encoding propagates invalid boundary JSON") {
  AnthropicAdapter adapter;
  auto value = request();
  value.messages.front().content = {
      ToolCallBlock{.id = "id", .name = "tool", .arguments = {.text = "{"}}};
  require_request_error(adapter, config(), value);
  value = request();
  value.messages.front().content = {
      ToolResultBlock{.tool_call_id = "id", .result = {.text = "{"}}};
  require_request_error(adapter, config(), value);
  value = request();
  value.tools.push_back(
      {.name = "tool", .description = "tool", .input_schema = {.text = "{"}});
  require_request_error(adapter, config(), value);
}
TEST_CASE("Anthropic HTTP errors cover status, correlation, and detail fallbacks") {
  AnthropicAdapter adapter;
  constexpr std::array statuses{
      std::pair{199, ErrorCategory::protocol},
      std::pair{599, ErrorCategory::network},
      std::pair{600, ErrorCategory::protocol},
  };
  for (const auto& [status, category] : statuses) {
    const auto result = adapter.parse_response(
        {.status_code = status},
        R"({"error":{"type":"safe_error"},"request_id":"body-id"})");
    REQUIRE_FALSE(result);
    CHECK(result.error().category == category);
    CHECK(result.error().provider_request_id == "body-id");
    CHECK(result.error().provider_detail == "anthropic:safe_error");
  }
  auto result = adapter.parse_response(
      {.status_code = 400, .provider_request_id = "header-id"}, "{");
  REQUIRE_FALSE(result);
  CHECK(result.error().provider_request_id == "header-id");
  CHECK(result.error().provider_detail.empty());
  for (const auto body : {R"({})", R"({"error":[]})", R"({"error":{}})",
                          R"({"error":{"type":null}})", R"({"error":{"type":7}})"}) {
    result = adapter.parse_response({.status_code = 400}, body);
    REQUIRE_FALSE(result);
    CHECK(result.error().provider_detail.empty());
  }
}
TEST_CASE("Anthropic successful responses reject malformed required shapes") {
  AnthropicAdapter adapter;
  constexpr std::array invalid{
      "{",
      R"({})",
      R"({"type":7})",
      R"({"type":"future","content":[]})",
      R"({"type":"message"})",
      R"({"type":"message","content":{}})",
      R"({"type":"message","content":[{"type":"future"}]})",
      R"({"type":"message","content":[],"stop_reason":7})",
      R"({"type":"message","content":[],"usage":[]})",
  };
  for (const auto body : invalid) {
    const auto result = adapter.parse_response({.status_code = 200}, body);
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::protocol);
  }
  const auto response = adapter.parse_response(
      {.status_code = 299},
      R"({"type":"message","request_id":"body-id","content":[{"type":"tool_use","id":"id","name":"lookup","input":{"x":1}}],"stop_reason":"tool_use","usage":{"input_tokens":4}})");
  REQUIRE(response);
  CHECK(response->finish_reason == FinishReason::tool_use);
  CHECK(response->provider_request_id == "body-id");
}
TEST_CASE("Anthropic stream start handles initial content and rejects bad envelopes") {
  AnthropicAdapter adapter;
  ProviderDecodeState state;
  state.response.provider_request_id = "header-id";
  auto result = event(
      adapter, "message_start",
      R"({"type":"message_start","message":{"type":"message","request_id":"body-id","content":[{"type":"text","text":"initial"},{"type":"tool_use","id":"id","name":"lookup","input":{}}],"stop_reason":null,"usage":{"input_tokens":2}}})",
      state);
  REQUIRE(result);
  REQUIRE(result->size() == 1);
  CHECK(std::get<ProviderTextDelta>(result->front()).text == "initial");
  CHECK(state.response.provider_request_id == "header-id");
  require_protocol(event(
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
    require_protocol(event(adapter, "message_start", body, fresh));
  }
}
TEST_CASE("Anthropic stream content start rejects invalid lifecycle and shapes") {
  AnthropicAdapter adapter;
  ProviderDecodeState empty;
  require_protocol(event(
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
    start_message(adapter, state);
    require_protocol(event(adapter, "content_block_start", body, state));
  }
  auto state = text_state(adapter);
  require_protocol(event(
      adapter, "content_block_start",
      R"({"type":"content_block_start","index":1,"content_block":{"type":"text","text":""}})",
      state));
  state = ProviderDecodeState{};
  start_message(adapter, state);
  state.finish_observed = true;
  require_protocol(event(
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
    require_protocol(event(adapter, "content_block_delta", body, state));
  }
  auto text = text_state(adapter);
  auto delta = event(
      adapter, "content_block_delta",
      R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"x"}})",
      text);
  REQUIRE(delta);
  CHECK(std::get<ProviderTextDelta>(delta->front()).text == "x");
  auto tool = tool_state(adapter);
  require_protocol(event(
      adapter, "content_block_delta",
      R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"x"}})",
      tool));
  for (
      const auto body :
      {R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta"}})",
       R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":7}})"}) {
    tool = tool_state(adapter);
    require_protocol(event(adapter, "content_block_delta", body, tool));
  }
}
TEST_CASE("Anthropic stream content stop canonicalizes tools and closes text") {
  AnthropicAdapter adapter;
  auto text = text_state(adapter);
  REQUIRE(event(adapter, "content_block_stop",
                R"({"type":"content_block_stop","index":0})", text));
  CHECK_FALSE(text.active_content_index);
  auto tool = tool_state(adapter);
  REQUIRE(event(adapter, "content_block_stop",
                R"({"type":"content_block_stop","index":0})", tool));
  CHECK(std::get<ToolCallBlock>(tool.response.content.front()).arguments.text == "{}");
  tool = tool_state(adapter);
  REQUIRE(event(
      adapter, "content_block_delta",
      R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{"}})",
      tool));
  require_protocol(event(adapter, "content_block_stop",
                         R"({"type":"content_block_stop","index":0})", tool));
}
TEST_CASE("Anthropic stream message finish enforces lifecycle and usage") {
  AnthropicAdapter adapter;
  ProviderDecodeState empty;
  require_protocol(
      event(adapter, "message_delta",
            R"({"type":"message_delta","delta":{"stop_reason":"end_turn"}})", empty));
  constexpr std::array invalid{
      R"({"type":"message_delta"})",
      R"({"type":"message_delta","delta":[]})",
      R"({"type":"message_delta","delta":{"stop_reason":7}})",
      R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":[]})",
  };
  for (const auto body : invalid) {
    ProviderDecodeState state;
    start_message(adapter, state);
    require_protocol(event(adapter, "message_delta", body, state));
  }
  ProviderDecodeState state;
  start_message(adapter, state);
  REQUIRE(event(adapter, "message_delta",
                R"({"type":"message_delta","delta":{"stop_reason":null}})", state));
  require_protocol(event(adapter, "message_stop", R"({"type":"message_stop"})", state));
  state = text_state(adapter);
  require_protocol(event(adapter, "message_stop", R"({"type":"message_stop"})", state));
  state = ProviderDecodeState{};
  start_message(adapter, state);
  REQUIRE(event(
      adapter, "message_delta",
      R"({"type":"message_delta","delta":{"stop_reason":"max_tokens"},"usage":{"output_tokens":3}})",
      state));
  REQUIRE(event(adapter, "message_stop", R"({"type":"message_stop"})", state));
  CHECK(state.response.finish_reason == FinishReason::length);
  CHECK(state.response.usage.output_tokens == 3);
}
TEST_CASE("Anthropic stream envelopes and provider errors cover safe categories") {
  AnthropicAdapter adapter;
  ProviderDecodeState state;
  require_protocol(event(adapter, "ping", R"({})", state));
  require_protocol(event(adapter, "ping", R"({"type":"message_stop"})", state));
  auto ignored = event(adapter, "message", R"({"type":"future"})", state);
  REQUIRE(ignored);
  CHECK(std::get<ProviderIgnoredEvent>(ignored->front()).name == "future");
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
                      R"("},"request_id":"id"})";
    auto result = event(adapter, "error", body, state);
    REQUIRE_FALSE(result);
    CHECK(result.error().category == category);
    CHECK(result.error().provider_request_id == "id");
    CHECK(result.error().retryable == (category == ErrorCategory::rate_limit ||
                                       category == ErrorCategory::network));
  }
  for (const auto body : {R"({"type":"error"})", R"({"type":"error","error":[]})",
                          R"({"type":"error","error":{"type":7}})"}) {
    auto result = event(adapter, "error", body, state);
    REQUIRE_FALSE(result);
    CHECK(result.error().provider_detail == "anthropic:unknown_error");
  }
}
