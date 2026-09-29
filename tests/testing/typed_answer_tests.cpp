// Typed turns end to end: the real worker, provider encoders and decoders, turn
// machine, and pump, with only the HTTP transfer scripted. Like the rest of this
// suite it reaches Scry through the installed public surface alone.
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <optional>
#include <scry/scry.hpp>
#include <scry/testing/scripted_transport.hpp>
#include <scry/testing/streams.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using scry::testing::ScriptedResponse;
using scry::testing::ScriptedTransport;

namespace {

struct Verdict {
  [[= scry::reflection::description{"Is the claim supported?"}]] bool supported{};
  std::string reason{};
};

struct LookupArguments {
  std::string topic{};
};

constexpr std::string_view valid_answer =
    R"({"supported":false,"reason":"It is rock."})";
constexpr std::string_view canonical_answer =
    R"({"reason":"It is rock.","supported":false})";

[[nodiscard]] scry::Config anthropic_config() {
  auto config = scry::Config{
      .base_url = "http://127.0.0.1:1",
      .api_key = "sanitized-test-key",
      .model = "test-model",
  };
  config.retry.max_attempts = 1;
  config.retry.jitter_ratio = 0.0;
  return config;
}

[[nodiscard]] scry::Config openai_config() {
  auto config = anthropic_config();
  config.base_url = "http://127.0.0.1:1/v1/";
  config.dialect = scry::ProviderDialect::openai_compatible;
  return config;
}

[[nodiscard]] ScriptedResponse scripted(std::string body) {
  return {.request_id = "scripted-request", .body_chunks = {std::move(body)}};
}

[[nodiscard]] scry::testing::ToolUseBlock respond(const std::string_view id,
                                                  const std::string_view arguments) {
  return {.id = id, .name = "respond", .arguments = arguments};
}

template <typename Value> [[nodiscard]] Value unwrap(scry::Result<Value> result) {
  REQUIRE(result);
  return std::move(*result);
}

[[nodiscard]] bool contains(const std::string& text, const std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

// Both dialects encode the committed answer as an assistant text block; a JSON
// string escapes its quotes.
[[nodiscard]] std::string escaped(const std::string_view json) {
  auto quoted = scry::escape_json_string(json);
  return quoted.substr(1, quoted.size() - 2);
}

template <typename Predicate>
[[nodiscard]] bool pump_until(scry::Harness& harness, Predicate&& predicate) {
  constexpr std::size_t maximum_pumps = 100'000;
  for (std::size_t pump = 0; pump < maximum_pumps; ++pump) {
    static_cast<void>(harness.update());
    if (predicate()) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

// The committed history holds no block that names the response tool, so the
// next request is valid whether or not it offers one.
void require_answer_only_as_text(const scry::Conversation& conversation) {
  for (const auto& message : conversation.messages()) {
    for (const auto& block : message.content) {
      const auto* call = std::get_if<scry::ToolCallBlock>(&block);
      CHECK((call == nullptr || call->name != "respond"));
    }
  }
  const auto& reply = conversation.messages().back();
  CHECK(reply.role == scry::Role::assistant);
  const auto* answer = std::get_if<scry::TextBlock>(&reply.content.back());
  REQUIRE(answer != nullptr);
  CHECK(answer->text == canonical_answer);
}

} // namespace

TEST_CASE("ask decodes an Anthropic answer and the next turn re-encodes it") {
  ScriptedTransport transport;
  transport.enqueue(scripted(scry::testing::anthropic_text_tool_stream(
      "Checked the almanac.", {respond("toolu_1", valid_answer)})));
  transport.enqueue(scripted(scry::testing::anthropic_text_stream("Anything else?")));
  auto harness = unwrap(scry::testing::create_harness(anthropic_config(), transport));
  auto conversation = unwrap(scry::Conversation::create());

  auto answered =
      unwrap(harness.ask<Verdict>(conversation, "Is the moon made of cheese?"));

  CHECK_FALSE(answered.value.supported);
  CHECK(answered.value.reason == "It is rock.");
  const auto& completion = answered.completion;
  CHECK(completion.finish_reason == scry::FinishReason::completed);
  CHECK(completion.text == "Checked the almanac.");
  REQUIRE(completion.structured);
  CHECK(completion.structured->text == canonical_answer);
  CHECK(completion.answer_attempt_count == 1);
  CHECK(completion.tool_call_count == 0);
  CHECK(completion.tool_round_count == 0);

  const auto first = transport.requests().front().body;
  CHECK(contains(first, R"("tool_choice":{"type":"any"})"));
  CHECK(contains(first, R"("name":"respond")"));
  CHECK(contains(first, "Is the claim supported?"));

  REQUIRE(conversation.message_count() == 2);
  require_answer_only_as_text(conversation);

  // A plain follow-up offers no response tool, forces nothing, and carries the
  // answer as the assistant's text.
  const auto follow_up = unwrap(harness.send_and_wait(conversation, "Thanks."));
  CHECK(follow_up.text == "Anything else?");
  CHECK_FALSE(follow_up.structured);
  const auto second = transport.requests().back().body;
  CHECK_FALSE(contains(second, "tool_choice"));
  CHECK_FALSE(contains(second, "respond"));
  CHECK_FALSE(contains(second, "tool_use"));
  CHECK(contains(second, escaped(canonical_answer)));
  CHECK(conversation.message_count() == 4);
}

TEST_CASE("send<T> completes an OpenAI-compatible answer reported with a plain stop") {
  ScriptedTransport transport;
  // A server forcing a tool call may finish with `stop` rather than `tool_calls`.
  transport.enqueue(scripted(scry::testing::openai_text_tool_stream(
      "Checked.", {respond("call_1", valid_answer)}, "chatcmpl-answer", "stop")));
  transport.enqueue(scripted(scry::testing::openai_text_stream("Anything else?")));
  auto harness = unwrap(scry::testing::create_harness(openai_config(), transport));
  auto conversation = unwrap(scry::Conversation::create());

  std::string streamed;
  std::optional<scry::Result<scry::Completion>> finished;
  auto turn = unwrap(harness.send<Verdict>(
      conversation, "Is the moon made of cheese?",
      {
          .on_text_delta = [&streamed](std::string_view text) { streamed += text; },
          .on_finished =
              [&finished](scry::Result<scry::Completion> result) {
                finished = std::move(result);
              },
      }));
  REQUIRE(pump_until(harness, [&turn] { return turn.finished(); }));

  REQUIRE(finished);
  REQUIRE(*finished);
  CHECK(streamed == "Checked.");
  CHECK((*finished)->text == "Checked.");
  REQUIRE((*finished)->structured);
  const auto verdict =
      unwrap(scry::reflection::decode<Verdict>(*(*finished)->structured));
  CHECK(verdict.reason == "It is rock.");
  CHECK(contains(transport.requests().front().body, R"("tool_choice":"required")"));
  require_answer_only_as_text(conversation);

  static_cast<void>(unwrap(harness.send_and_wait(conversation, "Thanks.")));
  const auto second = transport.requests().back().body;
  CHECK_FALSE(contains(second, "tool_choice"));
  CHECK_FALSE(contains(second, "tool_calls"));
  CHECK(contains(second, escaped(canonical_answer)));
}

TEST_CASE("an invalid answer goes back to the model, which corrects it") {
  ScriptedTransport transport;
  transport.enqueue(scripted(scry::testing::anthropic_tool_stream(
      {respond("toolu_1", R"({"supported":"no","reason":"It is rock."})")},
      "msg_first")));
  transport.enqueue(scripted(scry::testing::anthropic_tool_stream(
      {respond("toolu_2", valid_answer)}, "msg_second")));
  transport.enqueue(scripted(scry::testing::anthropic_text_stream("Anything else?")));
  auto harness = unwrap(scry::testing::create_harness(anthropic_config(), transport));
  auto conversation = unwrap(scry::Conversation::create());

  const auto answered =
      unwrap(harness.ask<Verdict>(conversation, "Is the moon made of cheese?"));

  CHECK(answered.value.reason == "It is rock.");
  CHECK(answered.completion.answer_attempt_count == 2);
  CHECK(answered.completion.tool_round_count == 1);
  CHECK(answered.completion.tool_call_count == 0);
  CHECK(answered.completion.attempt_count == 2);
  // The retry request carried the rejection, whose text names the path at fault.
  const auto requests = transport.requests();
  REQUIRE(requests.size() == 2);
  CHECK(contains(requests[1].body, R"("is_error":true,"tool_use_id":"toolu_1")"));
  CHECK(contains(requests[1].body, "$.supported"));
  // History keeps the question and the accepted answer, not the attempt.
  REQUIRE(conversation.message_count() == 2);
  require_answer_only_as_text(conversation);

  static_cast<void>(unwrap(harness.send_and_wait(conversation, "Thanks.")));
  CHECK_FALSE(contains(transport.requests().back().body, "toolu_1"));
}

TEST_CASE("an answer beside a real tool call waits for the tool") {
  ScriptedTransport transport;
  transport.enqueue(scripted(scry::testing::openai_text_tool_stream(
      "Let me look.",
      {
          {.id = "call_lookup", .name = "lookup", .arguments = R"({"topic":"moon"})"},
          respond("call_early", valid_answer),
      })));
  transport.enqueue(scripted(scry::testing::openai_tool_stream(
      {respond("call_answer", valid_answer)}, "chatcmpl-answer")));
  scry::ToolRegistry tools;
  REQUIRE(tools.add<LookupArguments>(
      {.name = "lookup", .description = "Look a topic up"},
      [](LookupArguments arguments) { return "notes on " + arguments.topic; }));
  auto harness = unwrap(
      scry::testing::create_harness(openai_config(), transport, std::move(tools)));
  auto conversation = unwrap(scry::Conversation::create());

  std::vector<std::string> requested;
  std::vector<std::string> observed;
  std::optional<scry::Result<scry::Completion>> finished;
  auto turn = unwrap(harness.send<Verdict>(
      conversation, "Is the moon made of cheese?",
      {
          .on_tool_request = [&requested](const scry::ToolRequest& request)
              -> std::optional<scry::ToolRejection> {
            requested.emplace_back(request.context.tool_name);
            return std::nullopt;
          },
          .on_tool_call =
              [&observed](const scry::ToolCall& call) {
                observed.push_back(call.name);
              },
          .on_finished =
              [&finished](scry::Result<scry::Completion> result) {
                finished = std::move(result);
              },
      }));
  REQUIRE(pump_until(harness, [&turn] { return turn.finished(); }));

  REQUIRE(finished);
  REQUIRE(*finished);
  // Only the real tool reached the host's hooks.
  CHECK(requested == std::vector<std::string>{"lookup"});
  CHECK(observed == std::vector<std::string>{"lookup"});
  CHECK((*finished)->tool_call_count == 1);
  CHECK((*finished)->answer_attempt_count == 2);
  CHECK((*finished)->tool_round_count == 1);
  const auto requests = transport.requests();
  REQUIRE(requests.size() == 2);
  CHECK(contains(requests[1].body,
                 "call respond exactly once, on its own, after your other tool calls "
                 "have returned"));
  // The real round stays in history without the refused answer.
  REQUIRE(conversation.message_count() == 4);
  const auto& round = conversation.messages()[1];
  REQUIRE(round.content.size() == 2);
  CHECK(std::get<scry::TextBlock>(round.content[0]).text == "Let me look.");
  CHECK(std::get<scry::ToolCallBlock>(round.content[1]).name == "lookup");
  require_answer_only_as_text(conversation);
}

TEST_CASE("a typed turn whose model calls no tool fails with protocol") {
  ScriptedTransport transport;
  transport.enqueue(scripted(scry::testing::anthropic_text_stream("No, it is rock.")));
  auto harness = unwrap(scry::testing::create_harness(anthropic_config(), transport));
  auto conversation = unwrap(scry::Conversation::create());

  const auto answered =
      harness.ask<Verdict>(conversation, "Is the moon made of cheese?");

  REQUIRE_FALSE(answered);
  CHECK(answered.error().category == scry::ErrorCategory::protocol);
  CHECK(
      contains(answered.error().message, "did not call the response tool \"respond\""));
  CHECK(conversation.message_count() == 0);
}

TEST_CASE("the round limit without a valid answer fails the typed turn") {
  ScriptedTransport transport;
  transport.enqueue(scripted(scry::testing::anthropic_tool_stream(
      {respond("toolu_1", R"({"supported":1})")}, "msg_first")));
  transport.enqueue(scripted(scry::testing::anthropic_tool_stream(
      {respond("toolu_2", R"({"supported":2})")}, "msg_second")));
  auto config = anthropic_config();
  config.max_tool_rounds = 1;
  config.tool_round_limit = scry::ToolRoundLimitPolicy::complete;
  auto harness = unwrap(scry::testing::create_harness(config, transport));
  auto conversation = unwrap(scry::Conversation::create());

  const auto answered =
      harness.ask<Verdict>(conversation, "Is the moon made of cheese?");

  REQUIRE_FALSE(answered);
  CHECK(answered.error().category == scry::ErrorCategory::max_tool_rounds);
  CHECK(conversation.message_count() == 0);
}

TEST_CASE("cancelling a typed turn from a callback commits nothing") {
  ScriptedTransport transport;
  transport.enqueue(scripted(scry::testing::anthropic_text_tool_stream(
      "Checked.", {respond("toolu_1", valid_answer)})));
  auto harness = unwrap(scry::testing::create_harness(anthropic_config(), transport));
  auto conversation = unwrap(scry::Conversation::create());

  std::optional<scry::TurnId> turn_id;
  std::optional<scry::Result<scry::Completion>> finished;
  auto turn = unwrap(
      harness.send<Verdict>(conversation, "Is the moon made of cheese?",
                            {
                                .on_text_delta =
                                    [&harness, &turn_id](std::string_view) {
                                      static_cast<void>(harness.cancel(*turn_id));
                                    },
                                .on_finished =
                                    [&finished](scry::Result<scry::Completion> result) {
                                      finished = std::move(result);
                                    },
                            }));
  turn_id = turn.id();
  REQUIRE(pump_until(harness, [&turn] { return turn.finished(); }));

  REQUIRE(finished);
  REQUIRE_FALSE(*finished);
  CHECK(finished->error().category == scry::ErrorCategory::cancelled);
  CHECK(conversation.message_count() == 0);
}

TEST_CASE("a dynamic response format names its tool and validates on the host") {
  ScriptedTransport transport;
  transport.enqueue(scripted(scry::testing::anthropic_tool_stream(
      {{.id = "toolu_1", .name = "verdict", .arguments = R"({"score":3})"}})));
  auto harness = unwrap(scry::testing::create_harness(anthropic_config(), transport));
  auto conversation = unwrap(scry::Conversation::create());

  std::vector<std::string> validated;
  const auto completion = unwrap(harness.send_and_wait_structured(
      conversation, "Score the claim.",
      scry::ResponseFormat{
          .name = "verdict",
          .schema =
              {.text =
                   R"({"type":"object","properties":{"score":{"type":"integer"}}})"},
          .validate = [&validated](const scry::Json& answer) -> scry::Status {
            validated.push_back(answer.text);
            return {};
          },
      }));

  CHECK(validated == std::vector<std::string>{R"({"score":3})"});
  REQUIRE(completion.structured);
  CHECK(completion.structured->text == R"({"score":3})");
  const auto body = transport.requests().front().body;
  CHECK(contains(body, R"("name":"verdict")"));
  // An empty description selects Scry's instruction for the response tool.
  CHECK(contains(body, "exactly once, on its own"));
}

TEST_CASE("send_structured rejects a response format that cannot be offered") {
  ScriptedTransport transport;
  scry::ToolRegistry tools;
  REQUIRE(tools.add<LookupArguments>({.name = "lookup", .description = "Look it up"},
                                     [](LookupArguments) { return 1; }));
  auto harness = unwrap(
      scry::testing::create_harness(anthropic_config(), transport, std::move(tools)));
  auto conversation = unwrap(scry::Conversation::create());

  const auto rejected = [&](scry::ResponseFormat format) {
    const auto turn =
        harness.send_structured(conversation, "question", std::move(format));
    REQUIRE_FALSE(turn);
    CHECK(turn.error().category == scry::ErrorCategory::invalid_argument);
    return turn.error().message;
  };
  CHECK(rejected({.name = "lookup", .schema = {.text = "{}"}}) ==
        "response format name \"lookup\" is a registered tool");
  CHECK(rejected({.name = "", .schema = {.text = "{}"}}) ==
        "response format name must not be empty");
  CHECK(rejected({.schema = {.text = "[]"}}) ==
        "response format schema must be a valid JSON object");
  // Nothing was accepted, so the Conversation is free and nothing was sent.
  CHECK(transport.calls() == 0);
  transport.enqueue(scripted(
      scry::testing::anthropic_tool_stream({respond("toolu_1", valid_answer)})));
  CHECK(harness.ask<Verdict>(conversation, "Is the moon made of cheese?"));
}

TEST_CASE("a conversation holding a typed answer survives persistence") {
  ScriptedTransport transport;
  transport.enqueue(scripted(scry::testing::anthropic_text_tool_stream(
      "Checked.", {respond("toolu_1", valid_answer)})));
  auto harness = unwrap(scry::testing::create_harness(anthropic_config(), transport));
  auto conversation = unwrap(scry::Conversation::create());
  static_cast<void>(
      unwrap(harness.ask<Verdict>(conversation, "Is the moon made of cheese?")));

  const auto saved = unwrap(conversation.to_json());
  auto restored = unwrap(scry::Conversation::from_json(saved));

  CHECK(unwrap(restored.to_json()).text == saved.text);
  REQUIRE(restored.message_count() == 2);
  require_answer_only_as_text(restored);
  // The restored history encodes for the other dialect too.
  ScriptedTransport other_transport;
  other_transport.enqueue(
      scripted(scry::testing::openai_text_stream("Anything else?")));
  auto other = unwrap(scry::testing::create_harness(openai_config(), other_transport));
  const auto follow_up = unwrap(other.send_and_wait(restored, "Thanks."));
  CHECK(follow_up.text == "Anything else?");
  CHECK(contains(other_transport.requests().back().body, escaped(canonical_answer)));
}
