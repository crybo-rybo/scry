#include "core/json_codec.hpp"
#include "core/model.hpp"
#include "core/provider.hpp"
#include "fixture_support.hpp"
#include "provider/anthropic.hpp"
#include "provider/anthropic_content.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <string>
#include <string_view>
#include <variant>

namespace {

using namespace scry;
using namespace scry::detail;
using namespace scry::test_fixtures;

void start_message(AnthropicAdapter& adapter, ProviderDecodeState& state) {
  REQUIRE(decode(
      adapter, "message_start",
      R"({"type":"message_start","message":{"type":"message","content":[],"stop_reason":null,"usage":{"input_tokens":5}}})",
      state));
}

void start_tool(AnthropicAdapter& adapter, ProviderDecodeState& state,
                const std::size_t index, const std::string_view id,
                const std::string_view name) {
  const auto payload =
      std::string{R"({"type":"content_block_start","index":)"} + std::to_string(index) +
      R"(,"content_block":{"type":"tool_use","id":")" + std::string{id} +
      R"(","name":")" + std::string{name} + R"(","input":{}}})";
  REQUIRE(decode(adapter, "content_block_start", payload, state));
}

void stop_tool(AnthropicAdapter& adapter, ProviderDecodeState& state,
               const std::size_t index) {
  const auto payload = std::string{R"({"type":"content_block_stop","index":)"} +
                       std::to_string(index) + "}";
  REQUIRE(decode(adapter, "content_block_stop", payload, state));
}

void append_arguments(AnthropicAdapter& adapter, ProviderDecodeState& state,
                      const std::size_t index, const std::string_view encoded_json) {
  const auto payload = std::string{R"({"type":"content_block_delta","index":)"} +
                       std::to_string(index) +
                       R"(,"delta":{"type":"input_json_delta","partial_json":")" +
                       std::string{encoded_json} + R"("}})";
  REQUIRE(decode(adapter, "content_block_delta", payload, state));
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

  REQUIRE(decode(
      adapter, "message_delta",
      R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":9}})",
      state));
  const auto completed =
      decode(adapter, "message_stop", R"({"type":"message_stop"})", state);
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
    const auto over = decode(
        adapter, "content_block_delta",
        R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":" "}})",
        state);
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
    const auto over = decode(
        adapter, "content_block_delta",
        R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{\"x\":1}"}})",
        state);
    REQUIRE_FALSE(over);
    CHECK(over.error().category == ErrorCategory::resource_limit);
    CHECK(
        std::get<ToolCallBlock>(state.response.content.front()).arguments.text.empty());
  }
}

TEST_CASE("Anthropic streamed tool arguments are validated but not rewritten") {
  AnthropicAdapter adapter;
  ProviderDecodeState state;
  start_message(adapter, state);
  start_tool(adapter, state, 0, "call-1", "lookup");
  append_arguments(adapter, state, 0, R"({ \"b\" : 2, \"a\" : 1 })");
  stop_tool(adapter, state, 0);

  // The stream layer proves the object root and forwards the received bytes;
  // TurnMachine is the one place that canonicalizes them.
  CHECK(std::get<ToolCallBlock>(state.response.content.front()).arguments.text ==
        R"({ "b" : 2, "a" : 1 })");
}

TEST_CASE("Anthropic stream rejects malformed JSON assembled from valid deltas") {
  AnthropicAdapter adapter;
  ProviderDecodeState state;
  start_message(adapter, state);
  start_tool(adapter, state, 0, "call-1", "lookup");
  append_arguments(adapter, state, 0, R"({\"x\":)");
  append_arguments(adapter, state, 0, "]}");

  require_protocol(decode(adapter, "content_block_stop",
                          R"({"type":"content_block_stop","index":0})", state));
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
    require_protocol(decode(adapter, "content_block_stop",
                            R"({"type":"content_block_stop","index":0})", state));
  }

  SECTION("non-streaming input") {
    const auto decoded = decode_anthropic_content(
        json_value(R"({"type":"tool_use","id":"call-1","name":"lookup","input":[]})"),
        false);
    REQUIRE_FALSE(decoded);
    CHECK(decoded.error().category == ErrorCategory::protocol);
  }
}
