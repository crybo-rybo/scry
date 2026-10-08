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

TEST_CASE("send<T> completes an OpenAI-compatible answer that persists and replays") {
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

  // The answered history round-trips through persistence and replays as text.
  const auto saved = unwrap(conversation.to_json());
  auto restored = unwrap(scry::Conversation::from_json(saved));
  CHECK(unwrap(restored.to_json()).text == saved.text);
  require_answer_only_as_text(restored);
  static_cast<void>(unwrap(harness.send_and_wait(restored, "Thanks.")));
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
}

// The prose beside a rejected answer stays in history as an assistant message of
// its own, directly before the assistant message that carries the accepted
// answer. The Messages API takes one message per role turn, so the next request
// must merge the two.
TEST_CASE("an Anthropic follow-up after a rejected answer alternates roles") {
  ScriptedTransport transport;
  transport.enqueue(scripted(scry::testing::anthropic_text_tool_stream(
      "Let me answer.",
      {respond("toolu_1", R"({"supported":"no","reason":"It is rock."})")},
      "msg_first")));
  transport.enqueue(scripted(scry::testing::anthropic_text_tool_stream(
      "Checked the almanac.", {respond("toolu_2", valid_answer)}, "msg_second")));
  transport.enqueue(scripted(scry::testing::anthropic_text_stream("Anything else?")));
  auto harness = unwrap(scry::testing::create_harness(anthropic_config(), transport));
  auto conversation = unwrap(scry::Conversation::create());

  const auto answered =
      unwrap(harness.ask<Verdict>(conversation, "Is the moon made of cheese?"));
  CHECK(answered.completion.answer_attempt_count == 2);
  CHECK(answered.completion.text == "Checked the almanac.");
  REQUIRE(conversation.message_count() == 3);
  require_answer_only_as_text(conversation);

  static_cast<void>(unwrap(harness.send_and_wait(conversation, "Thanks.")));
  const auto follow_up = transport.requests().back().body;
  const auto body = unwrap(scry::JsonView::parse(scry::Json{.text = follow_up}));
  const auto messages = body.find("messages");
  REQUIRE(messages);
  std::vector<std::string> roles;
  for (std::size_t index = 0; index < messages->size(); ++index) {
    const auto role = messages->at(index).value().find("role").value().string();
    REQUIRE(role);
    roles.emplace_back(*role);
  }
  CHECK(roles == std::vector<std::string>{"user", "assistant", "user"});
  CHECK_FALSE(contains(follow_up, "respond"));
  CHECK_FALSE(contains(follow_up, "tool_use"));
  CHECK(contains(follow_up, "Let me answer."));
  CHECK(contains(follow_up, escaped(canonical_answer)));
}

// Past max_tool_calls_per_turn a plain turn is told to answer without tools, but
// a typed turn still requires a tool call and can only end on its response tool,
// so the refusal names that tool. A dynamic format with its own name and host
// validator shows the name is the turn's; ask<T> reaches the same path.
TEST_CASE("the per-turn call limit tells a typed turn to finish on its response tool") {
  ScriptedTransport transport;
  transport.enqueue(scripted(scry::testing::openai_tool_stream({
      {.id = "call_first", .name = "lookup", .arguments = R"({"topic":"moon"})"},
      {.id = "call_second", .name = "lookup", .arguments = R"({"topic":"rock"})"},
  })));
  transport.enqueue(scripted(scry::testing::openai_tool_stream(
      {{.id = "call_answer", .name = "verdict", .arguments = valid_answer}},
      "chatcmpl-answer")));
  scry::ToolRegistry tools;
  REQUIRE(tools.add<LookupArguments>(
      {.name = "lookup", .description = "Look a topic up"},
      [](LookupArguments arguments) { return "notes on " + arguments.topic; }));
  auto config = openai_config();
  config.max_tool_calls_per_turn = 1;
  auto harness =
      unwrap(scry::testing::create_harness(config, transport, std::move(tools)));
  auto conversation = unwrap(scry::Conversation::create());

  std::vector<std::string> validated;
  const auto completion = unwrap(harness.send_and_wait_structured(
      conversation, "Is the moon made of cheese?",
      scry::ResponseFormat{
          .name = "verdict",
          .schema = {.text = std::string{scry::reflection::input_schema_v<Verdict>}},
          .validate = [&validated](const scry::Json& answer) -> scry::Status {
            validated.push_back(answer.text);
            return {};
          },
      }));

  CHECK(completion.rejected_tool_call_count == 1);
  REQUIRE(completion.structured);
  // The host's validator saw the canonical answer once.
  CHECK(validated == std::vector<std::string>{std::string{canonical_answer}});
  const auto requests = transport.requests();
  REQUIRE(requests.size() == 2);
  CHECK(contains(requests[0].body, R"("name":"verdict")"));
  // An empty description selects Scry's instruction for the response tool.
  CHECK(contains(requests[0].body, "exactly once, on its own"));
  const auto& follow_up = requests[1].body;
  CHECK(contains(follow_up, R"("tool_choice":"required")"));
  CHECK(contains(follow_up, "tool call limit for this turn reached; call verdict on "
                            "its own with your final answer"));
  CHECK_FALSE(contains(follow_up, "respond without calling tools"));
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
