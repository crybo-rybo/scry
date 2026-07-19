#include "core/model.hpp"
#include "core/provider.hpp"
#include "protocol/sse.hpp"

#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace scry;
using namespace scry::detail;

[[nodiscard]] std::string stream_fixture() {
  std::ifstream input{std::string{SCRY_ANTHROPIC_FIXTURE_DIR} + "/stream.sse"};
  REQUIRE(input.good());
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void append(std::vector<ProviderEvent>& destination,
            std::vector<ProviderEvent> source) {
  destination.insert(destination.end(), std::make_move_iterator(source.begin()),
                     std::make_move_iterator(source.end()));
}

[[nodiscard]] std::vector<ProviderEvent> decode_stream(ProviderAdapter& adapter,
                                                       ProviderDecodeState& state,
                                                       const std::string_view stream) {
  SseParser parser{256 * 1024};
  std::vector<ProviderEvent> result{};
  for (std::size_t offset = 0; offset < stream.size(); ++offset) {
    auto parsed = parser.push(stream.substr(offset, 1));
    REQUIRE(parsed.has_value());
    for (const auto& event : *parsed) {
      auto decoded = adapter.parse_stream_event(event.name, event.data, state);
      REQUIRE(decoded.has_value());
      append(result, std::move(*decoded));
    }
  }
  const auto tail = parser.finish();
  REQUIRE(tail.has_value());
  for (const auto& event : *tail) {
    auto decoded = adapter.parse_stream_event(event.name, event.data, state);
    REQUIRE(decoded.has_value());
    append(result, std::move(*decoded));
  }
  return result;
}

} // namespace

TEST_CASE("Anthropic stream decoder preserves deltas, usage, and completion") {
  auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter.has_value());
  ProviderDecodeState state{};
  state.response.provider_request_id = "req_stream_header";

  const auto events = decode_stream(**adapter, state, stream_fixture());
  REQUIRE(events.size() == 4);
  REQUIRE(std::holds_alternative<ProviderIgnoredEvent>(events[0]));
  CHECK(std::get<ProviderIgnoredEvent>(events[0]).name == "ping");
  CHECK(std::get<ProviderTextDelta>(events[1]).text == "Hello ");
  CHECK(std::get<ProviderTextDelta>(events[2]).text == "stream.");

  const auto& completed = std::get<ProviderCompleted>(events[3]).response;
  REQUIRE(completed.content.size() == 1);
  CHECK(std::get<TextBlock>(completed.content.front()).text == "Hello stream.");
  CHECK(completed.finish_reason == FinishReason::completed);
  CHECK(completed.usage.input_tokens == 12);
  CHECK(completed.usage.output_tokens == 3);
  CHECK(completed.provider_request_id == "req_stream_header");
  CHECK(state.semantic_output_consumed);
  CHECK(state.completed);
}

TEST_CASE("Anthropic stream decoder observes optional unknown events") {
  auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter.has_value());
  ProviderDecodeState state{};

  auto event = (*adapter)->parse_stream_event("future_optional", "not-json", state);
  REQUIRE(event.has_value());
  REQUIRE(event->size() == 1);
  CHECK(std::get<ProviderIgnoredEvent>(event->front()).name == "future_optional");

  event = (*adapter)->parse_stream_event(
      "message", R"({"type":"future_optional","value":1})", state);
  REQUIRE(event.has_value());
  REQUIRE(event->size() == 1);
  CHECK(std::get<ProviderIgnoredEvent>(event->front()).name == "future_optional");
}

TEST_CASE("Anthropic stream rejects decode state owned by another dialect") {
  auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter.has_value());
  ProviderDecodeState state{};
  state.dialect.emplace<OpenAiProviderDecodeState>();

  const auto event =
      (*adapter)->parse_stream_event("future_optional", "not-json", state);
  REQUIRE_FALSE(event);
  CHECK(event.error().category == ErrorCategory::protocol);
}

TEST_CASE("Anthropic stream decoder rejects malformed required events") {
  auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter.has_value());
  ProviderDecodeState state{};

  auto event = (*adapter)->parse_stream_event("content_block_delta", "{broken", state);
  REQUIRE_FALSE(event.has_value());
  CHECK(event.error().category == ErrorCategory::protocol);

  event = (*adapter)->parse_stream_event(
      "message_start",
      R"({"type":"message_start","message":{"type":"message","content":[],"stop_reason":null}})",
      state);
  REQUIRE(event.has_value());
  event = (*adapter)->parse_stream_event(
      "content_block_start",
      R"({"type":"content_block_start","index":0,"content_block":{"type":"future_required"}})",
      state);
  REQUIRE_FALSE(event.has_value());
  CHECK(event.error().category == ErrorCategory::protocol);

  event = (*adapter)->parse_stream_event("ping", R"({"type":"message_stop"})", state);
  REQUIRE_FALSE(event.has_value());
  CHECK(event.error().category == ErrorCategory::protocol);
}

TEST_CASE("Anthropic stream decoder preserves fragmented tool input shape") {
  auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter.has_value());
  ProviderDecodeState state{};

  const auto message_start = (*adapter)->parse_stream_event(
      "message_start",
      R"({"type":"message_start","message":{"type":"message","content":[],"stop_reason":null}})",
      state);
  REQUIRE(message_start.has_value());
  const auto start = (*adapter)->parse_stream_event(
      "content_block_start",
      R"({"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"tool_1","name":"lookup","input":{}}})",
      state);
  REQUIRE(start.has_value());
  const auto first = (*adapter)->parse_stream_event(
      "content_block_delta",
      R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{\"answer\":"}})",
      state);
  REQUIRE(first.has_value());
  const auto second = (*adapter)->parse_stream_event(
      "content_block_delta",
      R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"42}"}})",
      state);
  REQUIRE(second.has_value());
  const auto stop = (*adapter)->parse_stream_event(
      "content_block_stop", R"({"type":"content_block_stop","index":0})", state);
  REQUIRE(stop.has_value());

  const auto& tool = std::get<ToolCallBlock>(state.response.content.front());
  CHECK(tool.id == "tool_1");
  CHECK(tool.name == "lookup");
  CHECK(tool.arguments.text == R"({"answer":42})");
}

TEST_CASE("Anthropic stream decoder maps provider errors and terminal misuse") {
  auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter.has_value());
  ProviderDecodeState state{};

  auto event = (*adapter)->parse_stream_event(
      "error",
      R"({"type":"error","error":{"type":"overloaded_error","message":"private"},"request_id":"req_error"})",
      state);
  REQUIRE_FALSE(event.has_value());
  CHECK(event.error().category == ErrorCategory::network);
  CHECK(event.error().retryable);
  CHECK(event.error().provider_request_id == "req_error");
  CHECK(event.error().provider_detail == "anthropic:overloaded_error");
  CHECK(event.error().message.find("private") == std::string::npos);

  event = (*adapter)->parse_stream_event(
      "error",
      R"({"type":"error","error":{"type":"unsafe-secret-value","message":"private"}})",
      state);
  REQUIRE_FALSE(event.has_value());
  CHECK(event.error().category == ErrorCategory::protocol);
  CHECK(event.error().provider_detail == "anthropic:unknown_error");
  CHECK(event.error().provider_detail.find("secret") == std::string::npos);

  event = (*adapter)->parse_stream_event("message_stop", R"({"type":"message_stop"})",
                                         state);
  REQUIRE_FALSE(event.has_value());

  event = (*adapter)->parse_stream_event(
      "message_start",
      R"({"type":"message_start","message":{"type":"message","content":[],"stop_reason":null}})",
      state);
  REQUIRE(event.has_value());
  event = (*adapter)->parse_stream_event(
      "message_delta", R"({"type":"message_delta","delta":{"stop_reason":"end_turn"}})",
      state);
  REQUIRE(event.has_value());
  event = (*adapter)->parse_stream_event("message_stop", R"({"type":"message_stop"})",
                                         state);
  REQUIRE(event.has_value());
  event = (*adapter)->parse_stream_event("message_stop", R"({"type":"message_stop"})",
                                         state);
  REQUIRE_FALSE(event.has_value());
  CHECK(event.error().category == ErrorCategory::protocol);
}

TEST_CASE("Anthropic stream decoder rejects content after the finish event") {
  auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter.has_value());
  ProviderDecodeState state{};

  REQUIRE((*adapter)->parse_stream_event(
      "message_start",
      R"({"type":"message_start","message":{"type":"message","content":[],"stop_reason":null}})",
      state));
  REQUIRE((*adapter)->parse_stream_event(
      "message_delta", R"({"type":"message_delta","delta":{"stop_reason":"end_turn"}})",
      state));
  const auto late_content = (*adapter)->parse_stream_event(
      "content_block_start",
      R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})",
      state);

  REQUIRE_FALSE(late_content);
  CHECK(late_content.error().category == ErrorCategory::protocol);
}

TEST_CASE("Anthropic stream decoder enforces active block lifecycle boundaries") {
  auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter.has_value());
  ProviderDecodeState state{};

  REQUIRE((*adapter)->parse_stream_event(
      "message_start",
      R"({"type":"message_start","message":{"type":"message","content":[],"stop_reason":null}})",
      state));
  REQUIRE((*adapter)->parse_stream_event(
      "content_block_start",
      R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})",
      state));

  auto event = (*adapter)->parse_stream_event(
      "message_delta", R"({"type":"message_delta","delta":{"stop_reason":"end_turn"}})",
      state);
  REQUIRE_FALSE(event);
  CHECK(event.error().category == ErrorCategory::protocol);
  event = (*adapter)->parse_stream_event("message_stop", R"({"type":"message_stop"})",
                                         state);
  REQUIRE_FALSE(event);
  CHECK(event.error().category == ErrorCategory::protocol);

  REQUIRE((*adapter)->parse_stream_event(
      "content_block_stop", R"({"type":"content_block_stop","index":0})", state));
  event = (*adapter)->parse_stream_event(
      "content_block_delta",
      R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"late"}})",
      state);
  REQUIRE_FALSE(event);
  CHECK(event.error().category == ErrorCategory::protocol);

  REQUIRE((*adapter)->parse_stream_event(
      "message_delta", R"({"type":"message_delta","delta":{"stop_reason":"end_turn"}})",
      state));
  event = (*adapter)->parse_stream_event(
      "message_delta", R"({"type":"message_delta","delta":{"stop_reason":"end_turn"}})",
      state);
  REQUIRE_FALSE(event);
  CHECK(event.error().category == ErrorCategory::protocol);
}
