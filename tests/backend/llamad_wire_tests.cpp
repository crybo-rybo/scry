// The llamad wire mapping on its own, without a channel: request shapes the
// turn machine never produces but a hand-built ModelRequest can, and the
// decoder's event order.

#include "backend/llamad_wire.hpp"
#include "core/backend.hpp"
#include "core/model.hpp"
#include "core/provider.hpp"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace wire = ::llamad::v1;
using scry::detail::Message;
using scry::detail::ModelRequest;
using scry::detail::ProviderEvent;
using scry::detail::ProviderEventSink;
using scry::detail::ProviderSemanticOutput;
using scry::detail::ProviderTextDelta;
using scry::detail::llamad_wire::encode_chat_request;
using scry::detail::llamad_wire::StreamDecoder;

[[nodiscard]] ModelRequest request_of(std::vector<Message> messages) {
  return ModelRequest{.messages = std::move(messages)};
}

[[nodiscard]] Message user(std::vector<scry::ContentBlock> content) {
  return Message{.role = scry::Role::user, .content = std::move(content)};
}

[[nodiscard]] Message assistant(std::vector<scry::ContentBlock> content) {
  return Message{.role = scry::Role::assistant, .content = std::move(content)};
}

[[nodiscard]] scry::ToolCallBlock call(std::string id, std::string name,
                                       std::string arguments) {
  return scry::ToolCallBlock{.id = std::move(id),
                             .name = std::move(name),
                             .arguments = scry::Json{.text = std::move(arguments)}};
}

[[nodiscard]] scry::ToolResultBlock result(std::string id, std::string text) {
  return scry::ToolResultBlock{.tool_call_id = std::move(id),
                               .result = scry::Json{.text = std::move(text)}};
}

void require_invalid(const ModelRequest& request, const std::string_view message) {
  wire::ChatRequest out;
  const auto status = encode_chat_request(request, out);
  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::invalid_config);
  CHECK(status.error().message == message);
}

// Records what a decoder sends through its sink, in order.
struct RecordingSink {
  std::vector<ProviderEvent> events{};
  ProviderEventSink sink{[this](ProviderEvent event) -> scry::Status {
    events.push_back(std::move(event));
    return {};
  }};
};

[[nodiscard]] wire::GenerateChunk text_chunk(std::string text) {
  wire::GenerateChunk chunk;
  chunk.set_text(std::move(text));
  return chunk;
}

} // namespace

TEST_CASE("llamad encoding joins text blocks and tolerates an error-flagged result") {
  auto error_result = result("call_0", R"({"error":"no such city"})");
  error_result.is_error = true;
  const auto request = request_of({
      user({scry::TextBlock{.text = "one "}, scry::TextBlock{.text = "two"}}),
      assistant({scry::TextBlock{.text = "a"}, call("call_0", "lookup", "{}"),
                 scry::TextBlock{.text = "b"}}),
      user({error_result}),
  });
  wire::ChatRequest out;

  REQUIRE(encode_chat_request(request, out));

  REQUIRE(out.messages_size() == 3);
  CHECK(out.messages(0).content() == "one two");
  CHECK(out.messages(1).content() == "ab");
  REQUIRE(out.messages(1).tool_calls_size() == 1);
  CHECK(out.messages(1).tool_calls(0).arguments_json() == "{}");
  // No wire field carries is_error; the result JSON already says it failed.
  CHECK(out.messages(2).role() == "tool");
  CHECK(out.messages(2).content() == R"({"error":"no such city"})");
}

TEST_CASE("llamad encoding rejects message shapes the wire cannot carry") {
  require_invalid(request_of({user({call("c", "lookup", "{}")})}),
                  "llamad user messages cannot contain tool calls");
  require_invalid(
      request_of({user({scry::TextBlock{.text = "hi"}, result("c", "{}")})}),
      "llamad user messages cannot mix text and tool results");
  require_invalid(request_of({user({result("", "{}")})}),
                  "llamad tool results require a nonempty call ID");
  require_invalid(request_of({user({result("c", "{")})}),
                  "llamad tool result must be valid JSON");
  require_invalid(request_of({assistant({result("c", "{}")})}),
                  "llamad assistant messages cannot contain tool results");
  require_invalid(request_of({assistant({call("", "lookup", "{}")})}),
                  "llamad assistant tool calls require nonempty IDs and names");
  require_invalid(request_of({assistant({call("c", "", "{}")})}),
                  "llamad assistant tool calls require nonempty IDs and names");
  require_invalid(request_of({assistant({call("c", "lookup", "[]")})}),
                  "llamad tool arguments must be a JSON object");
}

TEST_CASE("llamad encoding rejects an unusable tool definition") {
  auto unnamed = request_of({user({scry::TextBlock{.text = "hi"}})});
  unnamed.tools = std::make_shared<const std::vector<scry::ToolDefinition>>(
      std::vector{scry::ToolDefinition{.input_schema = scry::Json{.text = "{}"}}});
  require_invalid(unnamed, "llamad tools require a nonempty name");

  auto bad_schema = request_of({user({scry::TextBlock{.text = "hi"}})});
  bad_schema.tools =
      std::make_shared<const std::vector<scry::ToolDefinition>>(std::vector{
          scry::ToolDefinition{.name = "t", .input_schema = scry::Json{.text = "[]"}}});
  require_invalid(bad_schema, "llamad tool schema must be a JSON object");
}

TEST_CASE("the llamad decoder reports semantic output once, before the first text") {
  RecordingSink recording;
  StreamDecoder decoder{scry::ResourceLimits{}, recording.sink};

  REQUIRE(decoder.consume(text_chunk("")));
  CHECK(recording.events.empty());
  REQUIRE(decoder.consume(text_chunk("a")));
  REQUIRE(decoder.consume(text_chunk("b")));
  auto final_chunk = text_chunk("c");
  final_chunk.set_finish_reason(wire::FINISH_REASON_EOG);
  REQUIRE(decoder.consume(final_chunk));

  REQUIRE(recording.events.size() == 4);
  CHECK(std::holds_alternative<ProviderSemanticOutput>(recording.events[0]));
  CHECK(std::get<ProviderTextDelta>(recording.events[1]).text == "a");
  CHECK(std::get<ProviderTextDelta>(recording.events[2]).text == "b");
  CHECK(std::get<ProviderTextDelta>(recording.events[3]).text == "c");
  const auto response = decoder.finish();
  REQUIRE(response);
  REQUIRE(response->content.size() == 1);
  CHECK(std::get<scry::TextBlock>(response->content[0]).text == "abc");
  CHECK(response->finish_reason == scry::FinishReason::completed);
}

TEST_CASE("the llamad decoder reports semantic output for tool calls without text") {
  RecordingSink recording;
  StreamDecoder decoder{scry::ResourceLimits{}, recording.sink};
  wire::GenerateChunk final_chunk;
  final_chunk.set_finish_reason(wire::FINISH_REASON_TOOL_CALLS);
  auto* encoded = final_chunk.add_tool_calls();
  encoded->set_id("call_0");
  encoded->set_name("lookup");
  encoded->set_arguments_json(R"({"city":"Oslo"})");

  REQUIRE(decoder.consume(final_chunk));

  REQUIRE(recording.events.size() == 1);
  CHECK(std::holds_alternative<ProviderSemanticOutput>(recording.events[0]));
  const auto response = decoder.finish();
  REQUIRE(response);
  CHECK(response->finish_reason == scry::FinishReason::tool_use);
  REQUIRE(response->content.size() == 1);
  const auto& block = std::get<scry::ToolCallBlock>(response->content[0]);
  CHECK(block.id == "call_0");
  CHECK(block.name == "lookup");
  CHECK(block.arguments.text == R"({"city":"Oslo"})");
}

TEST_CASE("the llamad decoder refuses tool calls on a text chunk") {
  RecordingSink recording;
  StreamDecoder decoder{scry::ResourceLimits{}, recording.sink};
  auto chunk = text_chunk("x");
  auto* encoded = chunk.add_tool_calls();
  encoded->set_id("c");
  encoded->set_name("lookup");
  encoded->set_arguments_json("{}");

  const auto status = decoder.consume(chunk);

  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::protocol);
  CHECK(status.error().message ==
        "llamad stream sent tool calls before its final chunk");
}

TEST_CASE("a failing llamad sink aborts the decode with its error") {
  ProviderEventSink sink{[](ProviderEvent) -> scry::Status {
    return std::unexpected(scry::Error{.category = scry::ErrorCategory::resource_limit,
                                       .message = "queue full"});
  }};
  StreamDecoder decoder{scry::ResourceLimits{}, sink};

  const auto status = decoder.consume(text_chunk("x"));

  REQUIRE_FALSE(status);
  CHECK(status.error().message == "queue full");
}
