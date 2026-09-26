#include "core/model.hpp"
#include "core/provider.hpp"
#include "fixture_support.hpp"
#include "protocol/sse.hpp"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace scry;
using namespace scry::detail;
using namespace scry::test_fixtures;

[[nodiscard]] std::string stream_fixture() {
  return scry::test_fixtures::anthropic_fixture("stream.sse");
}

// Every decoded event of the whole response lands in one caller-owned sink, so
// this also pins the ordering the worker depends on: each call appends after
// what the previous ones left behind.
void decode_events(ProviderAdapter& adapter, ProviderDecodeState& state,
                   const std::vector<SseEvent>& events,
                   std::vector<ProviderEvent>& out) {
  for (const auto& event : events) {
    REQUIRE(adapter.parse_stream_event(event.name, event.data, state, out).has_value());
  }
}

[[nodiscard]] std::vector<ProviderEvent> decode_stream(ProviderAdapter& adapter,
                                                       ProviderDecodeState& state,
                                                       const std::string_view stream) {
  SseParser parser{256 * 1024};
  std::vector<ProviderEvent> result{};
  std::vector<SseEvent> events{};
  for (std::size_t offset = 0; offset < stream.size(); ++offset) {
    events.clear();
    REQUIRE(parser.push(stream.substr(offset, 1), events).has_value());
    decode_events(adapter, state, events, result);
  }
  events.clear();
  REQUIRE(parser.finish(events).has_value());
  decode_events(adapter, state, events, result);
  return result;
}

} // namespace

TEST_CASE("Anthropic stream decoder preserves deltas, usage, and completion") {
  const auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter);
  ProviderDecodeState state{};
  state.response.provider_request_id = "req_stream_header";

  const auto events = decode_stream(*adapter, state, stream_fixture());
  // The fixture's ping is ignored without producing an event.
  REQUIRE(events.size() == 3);
  CHECK(std::get<ProviderTextDelta>(events[0]).text == "Hello ");
  CHECK(std::get<ProviderTextDelta>(events[1]).text == "stream.");

  const auto& completed = std::get<ProviderCompleted>(events[2]).response;
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
  const auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter);
  ProviderDecodeState state{};

  auto event = decode(*adapter, "future_optional", "not-json", state);
  REQUIRE(event.has_value());
  CHECK(event->empty());

  event = decode(*adapter, "message", R"({"type":"future_optional","value":1})", state);
  REQUIRE(event.has_value());
  CHECK(event->empty());
}

TEST_CASE("Anthropic stream rejects decode state owned by another dialect") {
  const auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter);
  ProviderDecodeState state{};
  state.dialect.emplace<OpenAiProviderDecodeState>();

  const auto event = decode(*adapter, "future_optional", "not-json", state);
  REQUIRE_FALSE(event);
  CHECK(event.error().category == ErrorCategory::protocol);
}

TEST_CASE("Anthropic stream decoder rejects malformed required events") {
  const auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter);
  ProviderDecodeState state{};

  auto event = decode(*adapter, "content_block_delta", "{broken", state);
  REQUIRE_FALSE(event.has_value());
  CHECK(event.error().category == ErrorCategory::protocol);

  start_anthropic_message(*adapter, state);
  event = decode(
      *adapter, "content_block_start",
      R"({"type":"content_block_start","index":0,"content_block":{"type":"future_required"}})",
      state);
  REQUIRE_FALSE(event.has_value());
  CHECK(event.error().category == ErrorCategory::protocol);

  event = decode(*adapter, "ping", R"({"type":"message_stop"})", state);
  REQUIRE_FALSE(event.has_value());
  CHECK(event.error().category == ErrorCategory::protocol);
}

TEST_CASE("Anthropic stream decoder preserves fragmented tool input shape") {
  const auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter);
  ProviderDecodeState state{};

  start_anthropic_message(*adapter, state);
  const auto start = decode(
      *adapter, "content_block_start",
      R"({"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"tool_1","name":"lookup","input":{}}})",
      state);
  REQUIRE(start.has_value());
  const auto first = decode(
      *adapter, "content_block_delta",
      R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{\"answer\":"}})",
      state);
  REQUIRE(first.has_value());
  const auto second = decode(
      *adapter, "content_block_delta",
      R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"42}"}})",
      state);
  REQUIRE(second.has_value());
  const auto stop = decode(*adapter, "content_block_stop",
                           R"({"type":"content_block_stop","index":0})", state);
  REQUIRE(stop.has_value());

  const auto& tool = std::get<ToolCallBlock>(state.response.content.front());
  CHECK(tool.id == "tool_1");
  CHECK(tool.name == "lookup");
  CHECK(tool.arguments.text == R"({"answer":42})");
}

TEST_CASE("Anthropic stream decoder rejects terminal misuse") {
  const auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter);
  ProviderDecodeState state{};

  auto event = decode(*adapter, "message_stop", R"({"type":"message_stop"})", state);
  REQUIRE_FALSE(event.has_value());

  start_anthropic_message(*adapter, state);
  event =
      decode(*adapter, "message_delta",
             R"({"type":"message_delta","delta":{"stop_reason":"end_turn"}})", state);
  REQUIRE(event.has_value());
  event = decode(*adapter, "message_stop", R"({"type":"message_stop"})", state);
  REQUIRE(event.has_value());
  event = decode(*adapter, "message_stop", R"({"type":"message_stop"})", state);
  REQUIRE_FALSE(event.has_value());
  CHECK(event.error().category == ErrorCategory::protocol);
}

TEST_CASE("Anthropic stream decoder rejects content after the finish event") {
  const auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter);
  ProviderDecodeState state{};

  start_anthropic_message(*adapter, state);
  REQUIRE(decode(*adapter, "message_delta",
                 R"({"type":"message_delta","delta":{"stop_reason":"end_turn"}})",
                 state));
  const auto late_content = decode(
      *adapter, "content_block_start",
      R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})",
      state);

  REQUIRE_FALSE(late_content);
  CHECK(late_content.error().category == ErrorCategory::protocol);
}

TEST_CASE("Anthropic stream decoder enforces active block lifecycle boundaries") {
  const auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter);
  ProviderDecodeState state{};

  start_anthropic_message(*adapter, state);
  REQUIRE(decode(
      *adapter, "content_block_start",
      R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})",
      state));

  auto event =
      decode(*adapter, "message_delta",
             R"({"type":"message_delta","delta":{"stop_reason":"end_turn"}})", state);
  REQUIRE_FALSE(event);
  CHECK(event.error().category == ErrorCategory::protocol);
  event = decode(*adapter, "message_stop", R"({"type":"message_stop"})", state);
  REQUIRE_FALSE(event);
  CHECK(event.error().category == ErrorCategory::protocol);

  REQUIRE(decode(*adapter, "content_block_stop",
                 R"({"type":"content_block_stop","index":0})", state));
  event = decode(
      *adapter, "content_block_delta",
      R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"late"}})",
      state);
  REQUIRE_FALSE(event);
  CHECK(event.error().category == ErrorCategory::protocol);

  REQUIRE(decode(*adapter, "message_delta",
                 R"({"type":"message_delta","delta":{"stop_reason":"end_turn"}})",
                 state));
  event =
      decode(*adapter, "message_delta",
             R"({"type":"message_delta","delta":{"stop_reason":"end_turn"}})", state);
  REQUIRE_FALSE(event);
  CHECK(event.error().category == ErrorCategory::protocol);
}

TEST_CASE("Anthropic stream rejects events after the completion claims the response") {
  const auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter);
  ProviderDecodeState state{};

  const auto events = decode_stream(*adapter, state, stream_fixture());
  REQUIRE_FALSE(events.empty());
  REQUIRE(std::holds_alternative<ProviderCompleted>(events.back()));
  CHECK(state.completed);

  // The terminal event owns the accumulated response, so every later event must
  // be refused before anything reads the decode state's response again.
  const auto late = decode(
      *adapter, "content_block_delta",
      R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"late"}})",
      state);
  REQUIRE_FALSE(late);
  CHECK(late.error().category == ErrorCategory::protocol);
}
