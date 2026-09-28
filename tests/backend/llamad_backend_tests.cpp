// The llamad backend end to end: a real Harness, created through the public
// Harness::create with ProviderDialect::llamad, against a scripted fake daemon
// on a private Unix socket.

#include "support/harness_test_support.hpp"
#include "support/llamad/fake_daemon.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace scry::test_support;
using namespace std::chrono_literals;
using scry::test::FakeDaemon;
using scry::test::FakeRound;
using scry::test::FakeToolCall;

namespace {

namespace wire = ::llamad::v1;

[[nodiscard]] scry::Config llamad_config(const std::string& target) {
  auto config = scry::Config{
      .base_url = target,
      .model = "local",
      .dialect = scry::ProviderDialect::llamad,
  };
  config.retry.max_attempts = 1;
  config.retry.jitter_ratio = 0.0;
  config.timeouts.connect = 2s;
  config.timeouts.shutdown = 50ms;
  return config;
}

[[nodiscard]] FakeRound
text_round(std::vector<std::string> text,
           const wire::FinishReason reason = wire::FINISH_REASON_EOG) {
  return FakeRound{.text = std::move(text), .reason = reason};
}

[[nodiscard]] FakeRound tool_round(std::vector<FakeToolCall> calls,
                                   std::vector<std::string> text = {}) {
  return FakeRound{
      .text = std::move(text),
      .reason = wire::FINISH_REASON_TOOL_CALLS,
      .tool_calls = std::move(calls),
  };
}

[[nodiscard]] scry::ToolDefinition lookup_tool() {
  return scry::ToolDefinition{
      .name = "lookup",
      .description = "Look up the weather for a city",
      .input_schema = scry::Json{.text = R"({"properties":{"city":{"type":"string"}},)"
                                         R"("required":["city"],"type":"object"})"},
  };
}

struct TurnOutcome {
  std::string streamed{};
  std::optional<scry::Result<scry::Completion>> result{};
};

// Sends one message and pumps until its terminal callback, collecting the
// streamed text on the way.
[[nodiscard]] TurnOutcome run_turn(scry::Harness& harness,
                                   scry::Conversation& conversation, std::string text) {
  TurnOutcome outcome;
  auto turn = unwrap(harness.send(
      conversation, std::move(text),
      scry::TurnCallbacks{
          .on_text_delta =
              [&outcome](const std::string_view delta) { outcome.streamed += delta; },
          .on_finished =
              [&outcome](scry::Result<scry::Completion> result) {
                outcome.result = std::move(result);
              },
      }));
  REQUIRE(pump_until_deadline(
      harness, [&outcome] { return outcome.result.has_value(); }, 10s));
  return outcome;
}

[[nodiscard]] scry::Error failed_turn(scry::Config config,
                                      std::vector<FakeRound> script) {
  FakeDaemon daemon{std::move(script)};
  config.base_url = daemon.target();
  auto harness = unwrap(scry::Harness::create(std::move(config)));
  auto conversation = unwrap(scry::Conversation::create());
  auto outcome = run_turn(harness, conversation, "hi");
  REQUIRE_FALSE(*outcome.result);
  CHECK(conversation.empty());
  return outcome.result->error();
}

[[nodiscard]] scry::Error failed_turn(std::vector<FakeRound> script) {
  return failed_turn(llamad_config(""), std::move(script));
}

} // namespace

TEST_CASE("llamad text chunks stream to on_text_delta and complete the turn") {
  auto round = text_round({"Hello ", "wor"});
  round.final_text = "ld";
  round.prompt_tokens = 12;
  round.completion_tokens = 3;
  FakeDaemon daemon{{round}};
  auto harness = unwrap(scry::Harness::create(llamad_config(daemon.target())));
  auto conversation = unwrap(scry::Conversation::create());

  const auto outcome = run_turn(harness, conversation, "hi");

  REQUIRE(*outcome.result);
  const auto& completion = **outcome.result;
  CHECK(outcome.streamed == "Hello world");
  CHECK(completion.text == "Hello world");
  CHECK(completion.finish_reason == scry::FinishReason::completed);
  CHECK(completion.usage.input_tokens == 12);
  CHECK(completion.usage.output_tokens == 3);
  CHECK(completion.attempt_count == 1);
  CHECK(completion.provider_request_id.empty());
  CHECK(conversation.message_count() == 2);
  CHECK(daemon.calls() == 1);
}

TEST_CASE("llamad requests carry the system prompt, sampling, tools, and history") {
  FakeDaemon daemon{{text_round({"first answer"}), text_round({"second answer"})}};
  auto config = llamad_config(daemon.target());
  config.sampling.temperature = 0.25;
  config.sampling.top_p = 0.5;
  config.sampling.seed = 7;
  config.sampling.max_tokens = 64;
  auto harness = unwrap(scry::Harness::create(std::move(config)));
  REQUIRE(harness.tools().add(lookup_tool(), static_handler(R"({})")));
  auto conversation =
      unwrap(scry::Conversation::create({.system_prompt = "Be brief."}));

  REQUIRE(*run_turn(harness, conversation, "first question").result);
  REQUIRE(*run_turn(harness, conversation, "second question").result);

  const auto requests = daemon.requests();
  REQUIRE(requests.size() == 2);
  const auto& first = requests[0];
  REQUIRE(first.messages_size() == 2);
  CHECK(first.messages(0).role() == "system");
  CHECK(first.messages(0).content() == "Be brief.");
  CHECK(first.messages(1).role() == "user");
  CHECK(first.messages(1).content() == "first question");
  CHECK(first.messages(1).tool_calls_size() == 0);
  CHECK(first.messages(1).tool_call_id().empty());
  REQUIRE(first.tools_size() == 1);
  CHECK(first.tools(0).name() == "lookup");
  CHECK(first.tools(0).description() == "Look up the weather for a city");
  CHECK(
      first.tools(0).parameters_json_schema() ==
      R"({"properties":{"city":{"type":"string"}},"required":["city"],"type":"object"})");
  CHECK(first.response_json_schema().empty());
  const auto& sampling = first.sampling();
  REQUIRE(sampling.has_temperature());
  CHECK(sampling.temperature() == 0.25F);
  REQUIRE(sampling.has_top_p());
  CHECK(sampling.top_p() == 0.5F);
  REQUIRE(sampling.has_seed());
  CHECK(sampling.seed() == 7);
  REQUIRE(sampling.has_max_tokens());
  CHECK(sampling.max_tokens() == 64);
  CHECK_FALSE(sampling.has_top_k());
  CHECK_FALSE(sampling.has_min_p());
  CHECK(sampling.stop_size() == 0);

  // The second turn resends the committed history after the system prompt.
  const auto& second = requests[1];
  REQUIRE(second.messages_size() == 4);
  CHECK(second.messages(1).content() == "first question");
  CHECK(second.messages(2).role() == "assistant");
  CHECK(second.messages(2).content() == "first answer");
  CHECK(second.messages(2).tool_calls_size() == 0);
  CHECK(second.messages(3).role() == "user");
  CHECK(second.messages(3).content() == "second question");
}

TEST_CASE("llamad requests omit unset optional sampling values") {
  FakeDaemon daemon{{text_round({"ok"})}};
  auto config = llamad_config(daemon.target());
  config.sampling.max_tokens.reset();
  auto harness = unwrap(scry::Harness::create(std::move(config)));
  auto conversation = unwrap(scry::Conversation::create());

  REQUIRE(*run_turn(harness, conversation, "hi").result);

  const auto requests = daemon.requests();
  REQUIRE(requests.size() == 1);
  // No system prompt means no system message, and no tools means none sent.
  REQUIRE(requests[0].messages_size() == 1);
  CHECK(requests[0].messages(0).role() == "user");
  CHECK(requests[0].tools_size() == 0);
  const auto& sampling = requests[0].sampling();
  CHECK(sampling.has_temperature());
  CHECK(sampling.temperature() == 1.0F);
  CHECK_FALSE(sampling.has_top_p());
  CHECK_FALSE(sampling.has_seed());
  CHECK_FALSE(sampling.has_max_tokens());
}

TEST_CASE("a llamad tool round resends the assistant tool calls and the results in "
          "call order") {
  FakeDaemon daemon{{
      tool_round(
          {
              {.id = "call_a", .name = "lookup", .arguments = R"({"city":"Oslo"})"},
              {.id = "call_b", .name = "lookup", .arguments = R"({"city":"Bergen"})"},
          },
          {"Let me look."}),
      text_round({"Oslo is sunny; Bergen is wet."}),
  }};
  auto harness = unwrap(scry::Harness::create(llamad_config(daemon.target())));
  std::vector<std::string> handled;
  REQUIRE(harness.tools().add(
      lookup_tool(), [&handled](scry::Json arguments) -> scry::Result<scry::Json> {
        handled.push_back(arguments.text);
        return scry::Json{.text = handled.size() == 1 ? R"({"forecast":"sunny"})"
                                                      : R"({"forecast":"wet"})"};
      }));
  auto conversation = unwrap(scry::Conversation::create());

  const auto outcome = run_turn(harness, conversation, "weather?");

  REQUIRE(*outcome.result);
  const auto& completion = **outcome.result;
  CHECK(completion.text == "Oslo is sunny; Bergen is wet.");
  CHECK(completion.tool_round_count == 1);
  CHECK(completion.tool_call_count == 2);
  CHECK(completion.attempt_count == 2);
  CHECK(outcome.streamed == "Let me look.Oslo is sunny; Bergen is wet.");
  CHECK(handled ==
        std::vector<std::string>{R"({"city":"Oslo"})", R"({"city":"Bergen"})"});
  CHECK(conversation.message_count() == 4);

  const auto requests = daemon.requests();
  REQUIRE(requests.size() == 2);
  const auto& resend = requests[1];
  REQUIRE(resend.messages_size() == 4);
  CHECK(resend.messages(0).role() == "user");
  CHECK(resend.messages(0).content() == "weather?");

  const auto& assistant = resend.messages(1);
  CHECK(assistant.role() == "assistant");
  CHECK(assistant.content() == "Let me look.");
  REQUIRE(assistant.tool_calls_size() == 2);
  CHECK(assistant.tool_calls(0).id() == "call_a");
  CHECK(assistant.tool_calls(0).name() == "lookup");
  CHECK(assistant.tool_calls(0).arguments_json() == R"({"city":"Oslo"})");
  CHECK(assistant.tool_calls(1).id() == "call_b");
  CHECK(assistant.tool_calls(1).arguments_json() == R"({"city":"Bergen"})");

  CHECK(resend.messages(2).role() == "tool");
  CHECK(resend.messages(2).tool_call_id() == "call_a");
  CHECK(resend.messages(2).content() == R"({"forecast":"sunny"})");
  CHECK(resend.messages(3).role() == "tool");
  CHECK(resend.messages(3).tool_call_id() == "call_b");
  CHECK(resend.messages(3).content() == R"({"forecast":"wet"})");
  CHECK(resend.tools_size() == 1);
}

TEST_CASE("llamad tool calls with no text still end retry eligibility") {
  // The tool calls are semantic output on their own: a call whose status fails
  // after they arrived is reported, not retried from scratch.
  auto asking = tool_round(
      {{.id = "call_0", .name = "lookup", .arguments = R"({"city":"Oslo"})"}});
  asking.failure_after_final = grpc::Status{grpc::StatusCode::UNAVAILABLE, "crashed"};
  FakeDaemon daemon{{asking, text_round({"must not be requested"})}};
  auto config = llamad_config(daemon.target());
  config.retry.max_attempts = 3;
  config.retry.initial_backoff = 1ms;
  config.retry.max_backoff = 1ms;
  auto harness = unwrap(scry::Harness::create(std::move(config)));
  REQUIRE(
      harness.tools().add(lookup_tool(), static_handler(R"({"forecast":"sunny"})")));
  auto conversation = unwrap(scry::Conversation::create());

  const auto outcome = run_turn(harness, conversation, "weather?");

  REQUIRE_FALSE(*outcome.result);
  CHECK(outcome.result->error().category == scry::ErrorCategory::network);
  CHECK(outcome.result->error().retryable);
  CHECK(outcome.result->error().attempt == 1);
  CHECK(daemon.calls() == 1);
  CHECK(conversation.empty());
}

TEST_CASE("llamad finish reasons map onto Scry's") {
  struct Case {
    wire::FinishReason reason;
    scry::FinishReason expected;
  };
  for (const auto& [reason, expected] : {
           Case{wire::FINISH_REASON_EOG, scry::FinishReason::completed},
           Case{wire::FINISH_REASON_STOP, scry::FinishReason::completed},
           Case{wire::FINISH_REASON_LENGTH, scry::FinishReason::length},
       }) {
    INFO(wire::FinishReason_Name(reason));
    FakeDaemon daemon{{text_round({"text"}, reason)}};
    auto harness = unwrap(scry::Harness::create(llamad_config(daemon.target())));
    auto conversation = unwrap(scry::Conversation::create());
    const auto outcome = run_turn(harness, conversation, "hi");
    REQUIRE(*outcome.result);
    CHECK((*outcome.result)->finish_reason == expected);
  }
}

TEST_CASE("a llamad CANCELLED finish reason fails the turn as cancelled") {
  // The turn machine ends every cancelled attempt through its own cancel path.
  const auto error =
      failed_turn({text_round({"partial"}, wire::FINISH_REASON_CANCELLED)});
  CHECK(error.category == scry::ErrorCategory::cancelled);
  CHECK(error.message == "turn cancelled");
}

TEST_CASE("llamad final chunks that break the stream contract fail with protocol") {
  struct Case {
    std::string_view name;
    FakeRound round;
    std::string_view message;
  };
  const Case cases[] = {
      {"tool calls without the tool-calls reason",
       FakeRound{.text = {"text"},
                 .reason = wire::FINISH_REASON_EOG,
                 .tool_calls = {{.id = "c", .name = "lookup", .arguments = "{}"}}},
       "llamad stream sent tool calls without the tool-calls finish reason"},
      {"tool-calls reason without calls", tool_round({}),
       "llamad stream finished for tool calls without any"},
      {"unknown finish reason",
       text_round({"text"}, static_cast<wire::FinishReason>(42)),
       "llamad stream ended with an unknown finish reason"},
      {"tool call without an ID", tool_round({{.name = "lookup", .arguments = "{}"}}),
       "llamad tool call is missing its ID or name"},
      {"tool call without a name", tool_round({{.id = "c", .arguments = "{}"}}),
       "llamad tool call is missing its ID or name"},
      {"arguments that are not an object",
       tool_round({{.id = "c", .name = "lookup", .arguments = "[1]"}}),
       "llamad tool arguments must be a JSON object"},
      {"arguments that are not JSON",
       tool_round({{.id = "c", .name = "lookup", .arguments = "{"}}),
       "llamad tool arguments must be a JSON object"},
      {"stream without a final chunk",
       FakeRound{.text = {"partial"}, .end_without_final = true},
       "llamad stream ended without a final chunk"},
      {"chunk after the final chunk",
       FakeRound{.text = {"text"}, .chunk_after_final = true},
       "llamad stream sent a chunk after its final chunk"},
  };
  for (const auto& [name, round, message] : cases) {
    INFO(name);
    const auto error = failed_turn({round});
    CHECK(error.category == scry::ErrorCategory::protocol);
    CHECK(error.message == message);
    CHECK_FALSE(error.retryable);
  }
}

TEST_CASE("llamad reports negative token counts as zero") {
  auto round = text_round({"ok"});
  round.prompt_tokens = -5;
  round.completion_tokens = -1;
  FakeDaemon daemon{{round}};
  auto harness = unwrap(scry::Harness::create(llamad_config(daemon.target())));
  auto conversation = unwrap(scry::Conversation::create());

  const auto outcome = run_turn(harness, conversation, "hi");

  REQUIRE(*outcome.result);
  CHECK((*outcome.result)->usage.input_tokens == 0);
  CHECK((*outcome.result)->usage.output_tokens == 0);
}

TEST_CASE("llamad gRPC statuses map to fixed errors without the daemon's message") {
  struct Case {
    grpc::StatusCode code;
    scry::ErrorCategory category;
    bool retryable;
    std::string_view message;
    std::string_view detail;
  };
  const Case cases[] = {
      {grpc::StatusCode::UNAVAILABLE, scry::ErrorCategory::network, true,
       "llamad daemon is unavailable", "llamad:unavailable"},
      {grpc::StatusCode::DEADLINE_EXCEEDED, scry::ErrorCategory::network, true,
       "transfer timed out", "llamad:deadline_exceeded"},
      {grpc::StatusCode::ABORTED, scry::ErrorCategory::network, true,
       "llamad aborted the call", "llamad:aborted"},
      {grpc::StatusCode::RESOURCE_EXHAUSTED, scry::ErrorCategory::resource_limit, false,
       "llamad call exceeded a gRPC message size limit", "llamad:resource_exhausted"},
      {grpc::StatusCode::INVALID_ARGUMENT, scry::ErrorCategory::invalid_config, false,
       "llamad rejected the request", "llamad:invalid_argument"},
      {grpc::StatusCode::FAILED_PRECONDITION, scry::ErrorCategory::invalid_config,
       false, "llamad rejected the request", "llamad:failed_precondition"},
      {grpc::StatusCode::UNIMPLEMENTED, scry::ErrorCategory::invalid_config, false,
       "llamad rejected the request", "llamad:unimplemented"},
      {grpc::StatusCode::CANCELLED, scry::ErrorCategory::network, true,
       "llamad cancelled the call", "llamad:cancelled"},
      {grpc::StatusCode::INTERNAL, scry::ErrorCategory::network, true,
       "llamad call failed", "llamad:internal"},
      {grpc::StatusCode::UNKNOWN, scry::ErrorCategory::network, true,
       "llamad call failed", "llamad:unknown"},
      {grpc::StatusCode::PERMISSION_DENIED, scry::ErrorCategory::network, true,
       "llamad call failed", "llamad:permission_denied"},
  };
  for (const auto& expected : cases) {
    INFO(expected.detail);
    const auto error = failed_turn(
        {FakeRound{.failure = grpc::Status{expected.code, "secret prompt text"}}});
    CHECK(error.category == expected.category);
    CHECK(error.retryable == expected.retryable);
    CHECK(error.message == expected.message);
    CHECK(error.provider_detail == expected.detail);
    CHECK(error.message.find("secret") == std::string::npos);
    CHECK(error.attempt == 1);
  }
}

TEST_CASE("a retryable llamad status before any output is retried") {
  FakeDaemon daemon{{
      FakeRound{.failure = grpc::Status{grpc::StatusCode::UNAVAILABLE, "loading"}},
      text_round({"second time lucky"}),
  }};
  auto config = llamad_config(daemon.target());
  config.retry.max_attempts = 2;
  config.retry.initial_backoff = 1ms;
  config.retry.max_backoff = 1ms;
  auto harness = unwrap(scry::Harness::create(std::move(config)));
  auto conversation = unwrap(scry::Conversation::create());

  const auto outcome = run_turn(harness, conversation, "hi");

  REQUIRE(*outcome.result);
  CHECK((*outcome.result)->text == "second time lucky");
  CHECK((*outcome.result)->attempt_count == 2);
  CHECK(daemon.calls() == 2);
}

TEST_CASE("a retryable llamad status after streamed text is not retried") {
  FakeDaemon daemon{{
      FakeRound{.text = {"partial"},
                .failure = grpc::Status{grpc::StatusCode::UNAVAILABLE, "crashed"}},
      text_round({"must not be requested"}),
  }};
  auto config = llamad_config(daemon.target());
  config.retry.max_attempts = 3;
  config.retry.initial_backoff = 1ms;
  config.retry.max_backoff = 1ms;
  auto harness = unwrap(scry::Harness::create(std::move(config)));
  auto conversation = unwrap(scry::Conversation::create());

  const auto outcome = run_turn(harness, conversation, "hi");

  REQUIRE_FALSE(*outcome.result);
  CHECK(outcome.streamed == "partial");
  CHECK(outcome.result->error().category == scry::ErrorCategory::network);
  CHECK(outcome.result->error().retryable);
  CHECK(daemon.calls() == 1);
}

TEST_CASE("Turn::cancel ends a hanging llamad stream promptly") {
  FakeDaemon daemon{{FakeRound{.text = {"thinking"}, .hang = true}}};
  auto harness = unwrap(scry::Harness::create(llamad_config(daemon.target())));
  auto conversation = unwrap(scry::Conversation::create());
  std::string streamed;
  std::optional<scry::Result<scry::Completion>> outcome;
  auto turn = unwrap(harness.send(
      conversation, "hi",
      scry::TurnCallbacks{
          .on_text_delta =
              [&streamed](const std::string_view text) { streamed += text; },
          .on_finished =
              [&outcome](scry::Result<scry::Completion> r) { outcome = std::move(r); },
      }));
  REQUIRE(pump_until_deadline(
      harness, [&] { return daemon.hanging() && streamed == "thinking"; }, 5s));

  const auto cancelled_at = std::chrono::steady_clock::now();
  CHECK(turn.cancel());
  REQUIRE(pump_until_deadline(harness, [&outcome] { return outcome.has_value(); }, 5s));

  CHECK(std::chrono::steady_clock::now() - cancelled_at < 2s);
  REQUIRE_FALSE(*outcome);
  CHECK(outcome->error().category == scry::ErrorCategory::cancelled);
  CHECK(conversation.empty());
  REQUIRE(
      pump_until_deadline(harness, [&daemon] { return daemon.observed_cancel(); }, 5s));
}

TEST_CASE("destroying a Harness mid-stream releases the llamad call promptly") {
  FakeDaemon daemon{{FakeRound{.hang = true}}};
  auto conversation = unwrap(scry::Conversation::create());
  std::optional<scry::Harness> harness{
      unwrap(scry::Harness::create(llamad_config(daemon.target())))};
  auto turn = unwrap(harness->send(conversation, "hi"));
  REQUIRE(pump_until_deadline(*harness, [&daemon] { return daemon.hanging(); }, 5s));

  const auto started = std::chrono::steady_clock::now();
  harness.reset();
  const auto destruction = std::chrono::steady_clock::now() - started;

  CHECK(destruction < 2s);
  CHECK(conversation.empty());
}

TEST_CASE("a silent llamad stream fails with a retryable timeout after the idle "
          "bound") {
  auto config = llamad_config("");
  config.timeouts.idle = 200ms;
  config.timeouts.shutdown = 2s;
  const auto started = std::chrono::steady_clock::now();

  const auto error = failed_turn(std::move(config), {FakeRound{.hang = true}});

  const auto elapsed = std::chrono::steady_clock::now() - started;
  CHECK(error.category == scry::ErrorCategory::network);
  CHECK(error.retryable);
  CHECK(error.message == "transfer timed out");
  CHECK(error.provider_detail == "llamad:idle_timeout");
  // The idle bound, not the two-second shutdown poll, decides when it fires.
  CHECK(elapsed >= 200ms);
  CHECK(elapsed < 1500ms);
}

TEST_CASE("a llamad transfer bound ends a long call as a retryable timeout") {
  auto config = llamad_config("");
  config.timeouts.transfer = 200ms;
  const auto error = failed_turn(std::move(config), {FakeRound{.hang = true}});
  CHECK(error.category == scry::ErrorCategory::network);
  CHECK(error.retryable);
  CHECK(error.message == "transfer timed out");
  CHECK(error.provider_detail == "llamad:deadline_exceeded");
}

TEST_CASE("llamad tool arguments over the configured limit fail with resource_limit") {
  auto config = llamad_config("");
  config.limits.max_tool_arguments_bytes = 16;
  const auto error =
      failed_turn(std::move(config),
                  {tool_round({{.id = "c",
                                .name = "lookup",
                                .arguments = R"({"city":"Llanfairpwllgwyngyll"})"}})});
  CHECK(error.category == scry::ErrorCategory::resource_limit);
  CHECK(error.message == "llamad tool arguments exceed the configured byte limit");
}

TEST_CASE("llamad responses over the configured byte limit fail with resource_limit") {
  auto config = llamad_config("");
  config.limits.max_response_bytes = 64;
  const auto error = failed_turn(
      std::move(config), {text_round({std::string(20, 'a'), std::string(20, 'b'),
                                      std::string(20, 'c'), std::string(20, 'd')})});
  CHECK(error.category == scry::ErrorCategory::resource_limit);
  CHECK(error.message == "response exceeds configured limit");
}

TEST_CASE("a llamad socket nobody listens on fails fast with a retryable network "
          "error") {
  auto config = llamad_config(scry::test::missing_socket_target());
  config.timeouts.connect = 5s;
  auto harness = unwrap(scry::Harness::create(std::move(config)));
  auto conversation = unwrap(scry::Conversation::create());
  const auto started = std::chrono::steady_clock::now();

  const auto outcome = run_turn(harness, conversation, "hi");

  CHECK(std::chrono::steady_clock::now() - started < 3s);
  REQUIRE_FALSE(*outcome.result);
  CHECK(outcome.result->error().category == scry::ErrorCategory::network);
  CHECK(outcome.result->error().retryable);
  CHECK(outcome.result->error().message == "llamad daemon is unreachable");
  CHECK(outcome.result->error().provider_detail == "llamad:unavailable");
}
