#include "core/model.hpp"
#include "core/provider.hpp"
#include "provider/anthropic.hpp"
#include "provider/anthropic_content.hpp"
#include "provider/wire_json.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace scry;
using namespace scry::detail;

[[nodiscard]] Result<std::vector<ProviderEvent>> event(AnthropicAdapter& adapter,
                                                       ProviderDecodeState& state,
                                                       const std::string_view name,
                                                       const std::string_view data) {
  return adapter.parse_stream_event(name, data, state);
}

void start_message(AnthropicAdapter& adapter, ProviderDecodeState& state) {
  REQUIRE(event(
      adapter, state, "message_start",
      R"({"type":"message_start","message":{"type":"message","content":[],"stop_reason":null,"usage":{"input_tokens":5}}})"));
}

void start_tool(AnthropicAdapter& adapter, ProviderDecodeState& state,
                const std::size_t index, const std::string_view id,
                const std::string_view name) {
  const auto payload =
      std::string{R"({"type":"content_block_start","index":)"} + std::to_string(index) +
      R"(,"content_block":{"type":"tool_use","id":")" + std::string{id} +
      R"(","name":")" + std::string{name} + R"(","input":{}}})";
  REQUIRE(event(adapter, state, "content_block_start", payload));
}

void stop_tool(AnthropicAdapter& adapter, ProviderDecodeState& state,
               const std::size_t index) {
  const auto payload = std::string{R"({"type":"content_block_stop","index":)"} +
                       std::to_string(index) + "}";
  REQUIRE(event(adapter, state, "content_block_stop", payload));
}

void append_arguments(AnthropicAdapter& adapter, ProviderDecodeState& state,
                      const std::size_t index, const std::string_view encoded_json) {
  const auto payload = std::string{R"({"type":"content_block_delta","index":)"} +
                       std::to_string(index) +
                       R"(,"delta":{"type":"input_json_delta","partial_json":")" +
                       std::string{encoded_json} + R"("}})";
  REQUIRE(event(adapter, state, "content_block_delta", payload));
}

[[nodiscard]] std::string canonical_json(const std::string_view json) {
  auto parsed = parse_wire_json(json, ErrorCategory::protocol, "invalid test JSON");
  REQUIRE(parsed);
  auto encoded =
      write_wire_json(*parsed, ErrorCategory::protocol, "test JSON encode failed");
  REQUIRE(encoded);
  return *encoded;
}

[[nodiscard]] std::string canonical_value(const WireValue& value) {
  auto encoded =
      write_wire_json(value, ErrorCategory::protocol, "test JSON encode failed");
  REQUIRE(encoded);
  return canonical_json(*encoded);
}

[[nodiscard]] const WireValue& required_field(const WireValue& value,
                                              const std::string_view name) {
  const auto* field = wire_field(value, name);
  REQUIRE(field != nullptr);
  return *field;
}

[[nodiscard]] Config config() {
  return {
      .base_url = "https://api.anthropic.test",
      .api_key = "sanitized-key",
      .model = "claude-test",
  };
}

[[nodiscard]] ModelRequest multi_tool_request() {
  return {
      .model = "claude-test",
      .messages =
          {
              Message{
                  .role = Role::assistant,
                  .content =
                      {
                          ToolCallBlock{
                              .id = "call-a",
                              .name = "weather",
                              .arguments = Json{.text = R"({"city":"Paris"})"},
                          },
                          ToolCallBlock{
                              .id = "call-b",
                              .name = "days",
                              .arguments = Json{.text = R"({"days":3})"},
                          },
                      },
              },
              Message{
                  .role = Role::user,
                  .content =
                      {
                          ToolResultBlock{
                              .tool_call_id = "call-a",
                              .result = Json{.text = R"({"temperature":21})"},
                          },
                          ToolResultBlock{
                              .tool_call_id = "call-b",
                              .result = Json{.text = R"({"error":"closed"})"},
                              .is_error = true,
                          },
                      },
              },
          },
      .tools =
          {
              ToolSchema{
                  .name = "weather",
                  .description = "Get weather",
                  .input_schema =
                      Json{.text = R"({"type":"object","required":["city"]})"},
              },
              ToolSchema{
                  .name = "days",
                  .description = "Get days",
                  .input_schema =
                      Json{.text = R"({"type":"object","required":["days"]})"},
              },
          },
  };
}

void check_schemas(const WireValue& root) {
  const auto tools = required_wire_array(root, "tools");
  REQUIRE(tools);
  REQUIRE((*tools)->size() == 2);
  CHECK(*required_wire_string((*tools)->at(0), "name") == "weather");
  CHECK(*required_wire_string((*tools)->at(1), "name") == "days");
  CHECK(canonical_value(required_field((*tools)->at(0), "input_schema")) ==
        canonical_json(R"({"type":"object","required":["city"]})"));
  CHECK(canonical_value(required_field((*tools)->at(1), "input_schema")) ==
        canonical_json(R"({"type":"object","required":["days"]})"));
}

void check_messages(const WireValue& root) {
  const auto messages = required_wire_array(root, "messages");
  REQUIRE(messages);
  REQUIRE((*messages)->size() == 2);
  const auto calls = required_wire_array((*messages)->at(0), "content");
  REQUIRE(calls);
  REQUIRE((*calls)->size() == 2);
  CHECK(*required_wire_string((*calls)->at(0), "id") == "call-a");
  CHECK(*required_wire_string((*calls)->at(1), "id") == "call-b");

  const auto results = required_wire_array((*messages)->at(1), "content");
  REQUIRE(results);
  REQUIRE((*results)->size() == 2);
  CHECK(
      canonical_value((*results)->at(0)) ==
      canonical_json(
          R"({"type":"tool_result","tool_use_id":"call-a","content":"{\"temperature\":21}","is_error":false})"));
  CHECK(
      canonical_value((*results)->at(1)) ==
      canonical_json(
          R"({"type":"tool_result","tool_use_id":"call-b","content":"{\"error\":\"closed\"}","is_error":true})"));
}

} // namespace

TEST_CASE("Anthropic stream preserves multiple independently fragmented tool calls") {
  AnthropicAdapter adapter;
  ProviderDecodeState state;
  start_message(adapter, state);

  start_tool(adapter, state, 0, "call-weather", "weather");
  append_arguments(adapter, state, 0, R"({\"city\":)");
  append_arguments(adapter, state, 0, R"(\"Paris\"})");
  stop_tool(adapter, state, 0);

  start_tool(adapter, state, 1, "call-days", "days");
  append_arguments(adapter, state, 1, R"({\"days\":)");
  append_arguments(adapter, state, 1, "3}");
  stop_tool(adapter, state, 1);

  REQUIRE(event(
      adapter, state, "message_delta",
      R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":9}})"));
  const auto completed =
      event(adapter, state, "message_stop", R"({"type":"message_stop"})");
  REQUIRE(completed);
  REQUIRE(completed->size() == 1);

  const auto& response = std::get<ProviderCompleted>(completed->front()).response;
  REQUIRE(response.content.size() == 2);
  const auto& first = std::get<ToolCallBlock>(response.content[0]);
  const auto& second = std::get<ToolCallBlock>(response.content[1]);
  CHECK(first.id == "call-weather");
  CHECK(first.name == "weather");
  CHECK(first.arguments.text == R"({"city":"Paris"})");
  CHECK(second.id == "call-days");
  CHECK(second.name == "days");
  CHECK(second.arguments.text == R"({"days":3})");
  CHECK(response.finish_reason == FinishReason::tool_use);
  CHECK(response.usage.input_tokens == 5);
  CHECK(response.usage.output_tokens == 9);
}

TEST_CASE("Anthropic streamed argument limit rejects before appending") {
  AnthropicAdapter adapter;

  SECTION("exact boundary remains valid") {
    ProviderDecodeState state{.max_tool_arguments_bytes = 7};
    start_message(adapter, state);
    start_tool(adapter, state, 0, "call-1", "lookup");
    append_arguments(adapter, state, 0, R"({\"x\":)");
    append_arguments(adapter, state, 0, "1}");

    auto& arguments =
        std::get<ToolCallBlock>(state.response.content.front()).arguments.text;
    REQUIRE(arguments == R"({"x":1})");
    const auto over = event(
        adapter, state, "content_block_delta",
        R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":" "}})");
    REQUIRE_FALSE(over);
    CHECK(over.error().category == ErrorCategory::resource_limit);
    CHECK(arguments == R"({"x":1})");

    stop_tool(adapter, state, 0);
    CHECK(arguments == R"({"x":1})");
  }

  SECTION("first oversized fragment leaves the destination empty") {
    ProviderDecodeState state{.max_tool_arguments_bytes = 6};
    start_message(adapter, state);
    start_tool(adapter, state, 0, "call-1", "lookup");
    const auto over = event(
        adapter, state, "content_block_delta",
        R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{\"x\":1}"}})");
    REQUIRE_FALSE(over);
    CHECK(over.error().category == ErrorCategory::resource_limit);
    CHECK(
        std::get<ToolCallBlock>(state.response.content.front()).arguments.text.empty());
  }
}

TEST_CASE("Anthropic stream rejects malformed JSON assembled from valid deltas") {
  AnthropicAdapter adapter;
  ProviderDecodeState state;
  start_message(adapter, state);
  start_tool(adapter, state, 0, "call-1", "lookup");
  append_arguments(adapter, state, 0, R"({\"x\":)");
  append_arguments(adapter, state, 0, "]}");

  const auto stopped = event(adapter, state, "content_block_stop",
                             R"({"type":"content_block_stop","index":0})");
  REQUIRE_FALSE(stopped);
  CHECK(stopped.error().category == ErrorCategory::protocol);
  CHECK(std::get<ToolCallBlock>(state.response.content.front()).arguments.text ==
        R"({"x":]})");
}

TEST_CASE("Anthropic tool arguments require JSON object roots") {
  AnthropicAdapter adapter;

  SECTION("streamed input") {
    ProviderDecodeState state;
    start_message(adapter, state);
    start_tool(adapter, state, 0, "call-1", "lookup");
    append_arguments(adapter, state, 0, R"([1,2])");
    const auto stopped = event(adapter, state, "content_block_stop",
                               R"({"type":"content_block_stop","index":0})");
    REQUIRE_FALSE(stopped);
    CHECK(stopped.error().category == ErrorCategory::protocol);
  }

  SECTION("non-streaming input") {
    const auto parsed = parse_wire_json(
        R"({"type":"tool_use","id":"call-1","name":"lookup","input":[]})",
        ErrorCategory::protocol, "invalid test JSON");
    REQUIRE(parsed);
    const auto decoded = decode_anthropic_content(*parsed, false);
    REQUIRE_FALSE(decoded);
    CHECK(decoded.error().category == ErrorCategory::protocol);
  }
}

TEST_CASE("Anthropic request serializes multiple schemas and tool results in order") {
  AnthropicAdapter adapter;
  const auto encoded = adapter.make_request(config(), multi_tool_request());
  REQUIRE(encoded);
  auto root =
      parse_wire_json(encoded->body, ErrorCategory::protocol, "request is invalid");
  REQUIRE(root);
  check_schemas(*root);
  check_messages(*root);
}
