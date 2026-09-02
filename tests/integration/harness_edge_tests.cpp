#include "runtime/test_access.hpp"
#include "support/harness_test_support.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <optional>
#include <scry/scry.hpp>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace scry::test_support;

namespace {

const std::string completed_stream =
    anthropic_text_stream("coverage answer", "msg_coverage", {}, 3, 2);

const std::string correlated_stream =
    anthropic_text_stream("correlated", "msg_correlated", "stream-request", 3, 1);

constexpr std::string_view tool_stream = R"(event: message_start
data: {"type":"message_start","message":{"id":"msg_tool","type":"message","role":"assistant","content":[],"model":"test-model","stop_reason":null,"usage":{"input_tokens":3,"output_tokens":0}}}

event: content_block_start
data: {"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"call_1","name":"lookup","input":{}}}

event: content_block_stop
data: {"type":"content_block_stop","index":0}

event: message_delta
data: {"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":1}}

event: message_stop
data: {"type":"message_stop"}

)";

[[nodiscard]] scry::Result<scry::Harness>
fake_harness(scry::Config config, scry::test::ScriptedExchange scripted) {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  fake->enqueue(std::move(scripted));
  return scry::detail::HarnessTestAccess::create(std::move(config), provider(),
                                                 std::move(fake));
}

[[nodiscard]] std::string large_delta_stream() {
  auto stream = std::string{R"(event: message_start
data: {"type":"message_start","message":{"id":"msg_large","type":"message","role":"assistant","content":[],"model":"test-model","stop_reason":null,"usage":{"input_tokens":1,"output_tokens":0}}}

event: content_block_start
data: {"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}}

event: content_block_delta
data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":")"};
  stream.append(600, 'x');
  stream.append(R"("}}

event: content_block_stop
data: {"type":"content_block_stop","index":0}

event: message_delta
data: {"type":"message_delta","delta":{"stop_reason":"end_turn"}}

event: message_stop
data: {"type":"message_stop"}

)");
  return stream;
}

[[nodiscard]] scry::ToolDefinition tool() {
  return {
      .name = "coverage_tool",
      .description = "runtime coverage tool",
      .input_schema = {.text = R"({"type":"object"})"},
  };
}

class FailingProvider final : public scry::detail::ProviderAdapter {
public:
  [[nodiscard]] scry::Result<scry::detail::TransportRequest>
  make_request(const scry::Config&, const scry::detail::ModelRequest&) const override {
    return std::unexpected(scry::Error{
        .category = scry::ErrorCategory::protocol,
        .message = "provider could not construct the request",
    });
  }

  [[nodiscard]] scry::Result<std::vector<scry::detail::ProviderEvent>>
  parse_stream_event(std::string_view, std::string_view,
                     scry::detail::ProviderDecodeState&) const override {
    return {};
  }
};

class DuplicateCompletionProvider final : public scry::detail::ProviderAdapter {
public:
  [[nodiscard]] scry::Result<scry::detail::TransportRequest>
  make_request(const scry::Config& config,
               const scry::detail::ModelRequest&) const override {
    return scry::detail::TransportRequest{
        .url = config.base_url,
        .tls_verify_peer = config.tls_verify_peer,
        .timeouts = config.timeouts,
        .limits = config.limits,
    };
  }

  [[nodiscard]] scry::Result<std::vector<scry::detail::ProviderEvent>>
  parse_stream_event(std::string_view, std::string_view,
                     scry::detail::ProviderDecodeState&) const override {
    const auto completed = scry::detail::ProviderCompleted{
        .response =
            scry::detail::ModelResponse{
                .finish_reason = scry::FinishReason::completed,
            },
    };
    return std::vector<scry::detail::ProviderEvent>{completed, completed};
  }
};

// Holds one transfer open until the turn's cancel flag is set, so a test can
// address an in-flight turn by identifier. The shared fake's held exchange
// waits for an explicit release instead, so it cannot express this. The spin is bounded
// by the flag and by Harness shutdown; there is no sleep or wall-clock deadline.
class HeldTransport final : public scry::detail::Transport {
public:
  [[nodiscard]] scry::Result<scry::detail::TransportResult>
  perform(const scry::detail::TransportRequest&, const std::stop_token stopped,
          const std::atomic<bool>& cancelled, scry::detail::BodyChunkSink&) override {
    entered_.store(true, std::memory_order_release);
    while (!cancelled.load(std::memory_order_acquire) && !stopped.stop_requested()) {
      std::this_thread::yield();
    }
    return std::unexpected(scry::Error{
        .category = scry::ErrorCategory::cancelled,
        .message = "held transport cancelled",
    });
  }

  void wait_for_entry() const {
    while (!entered_.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }

private:
  std::atomic<bool> entered_{false};
};

// Deliberately violates the transport seam contract by throwing; the shared
// fake reports failures as values and cannot express this.
class ThrowingTransport final : public scry::detail::Transport {
public:
  [[nodiscard]] scry::Result<scry::detail::TransportResult>
  perform(const scry::detail::TransportRequest&, std::stop_token,
          const std::atomic<bool>&, scry::detail::BodyChunkSink&) override {
    throw std::runtime_error{"transport escaped its failure contract"};
  }
};

} // namespace

TEST_CASE("moved-from public runtime handles remain safely observable") {
  auto harness_result =
      fake_harness(test_config(), scripted_exchange(completed_stream));
  REQUIRE(harness_result);
  auto harness = std::move(*harness_result);
  auto conversation_result = scry::Conversation::create();
  REQUIRE(conversation_result);
  auto conversation = std::move(*conversation_result);

  CHECK(harness_result->update().callbacks_delivered == 0);
  CHECK(conversation_result->empty());
  CHECK(conversation_result->message_count() == 0);
  auto inactive_send = harness_result->send(conversation, "question");
  REQUIRE_FALSE(inactive_send);
  CHECK(inactive_send.error().category == scry::ErrorCategory::invalid_state);
  auto moved_conversation_send = harness.send(*conversation_result, "question");
  REQUIRE_FALSE(moved_conversation_send);

  const auto& const_tools = std::as_const(harness).tools();
  CHECK(const_tools.empty());
  REQUIRE(harness.tools().add(tool(), static_handler(R"({"ok":true})")));
  CHECK(const_tools.size() == 1);

  auto turn_result = harness.send(conversation, "question");
  REQUIRE(turn_result);
  auto turn = std::move(*turn_result);
  CHECK_FALSE(turn_result->id());
  CHECK_FALSE(turn_result->cancel());
  CHECK(turn.id());
}

TEST_CASE("a Turn can cancel safely after its Harness has been destroyed") {
  std::optional<scry::Turn> survivor;
  scry::TurnId accepted_id{};
  {
    auto fixture =
        make_harness_fixture(test_config(), {scripted_exchange(completed_stream)});
    auto turn = fixture.harness.send(fixture.conversation, "outlive the harness");
    REQUIRE(turn);
    accepted_id = turn->id();
    survivor.emplace(std::move(*turn));
  }

  REQUIRE(survivor);
  CHECK(survivor->id() == accepted_id);
  CHECK(survivor->cancel());
  CHECK_FALSE(survivor->cancel());
}

TEST_CASE("a completed turn publishes its history, busy state, and finished flag") {
  auto harness_result =
      fake_harness(test_config(), scripted_exchange(completed_stream));
  REQUIRE(harness_result);
  auto harness = std::move(*harness_result);
  auto conversation_result = scry::Conversation::create({.system_prompt = "Be brief."});
  REQUIRE(conversation_result);
  auto conversation = std::move(*conversation_result);
  CHECK(conversation.system_prompt() == "Be brief.");
  CHECK_FALSE(conversation.busy());

  bool finished = false;
  auto turn_result =
      harness.send(conversation, "question",
                   {
                       .on_finished =
                           [&finished](scry::Result<scry::Completion> outcome) {
                             finished = outcome.has_value();
                           },
                   });
  REQUIRE(turn_result);
  const auto turn = std::move(*turn_result);
  CHECK(conversation.busy());
  CHECK_FALSE(turn.finished());

  REQUIRE(pump_until(harness, [&finished] { return finished; }));
  CHECK(turn.finished());
  CHECK_FALSE(conversation.busy());

  const auto& messages = conversation.messages();
  REQUIRE(messages.size() == 2);
  CHECK(messages[0].role == scry::Role::user);
  const auto* asked = std::get_if<scry::TextBlock>(&messages[0].content.at(0));
  REQUIRE(asked != nullptr);
  CHECK(asked->text == "question");
  CHECK(messages[1].role == scry::Role::assistant);
  const auto* answered = std::get_if<scry::TextBlock>(&messages[1].content.at(0));
  REQUIRE(answered != nullptr);
  CHECK(answered->text == "coverage answer");
}

TEST_CASE("a turn without on_finished still reports finished once update runs") {
  auto fixture =
      make_harness_fixture(test_config(), {scripted_exchange(completed_stream)});

  auto turn_result = fixture.harness.send(fixture.conversation, "question");
  REQUIRE(turn_result);
  const auto turn = std::move(*turn_result);
  CHECK_FALSE(turn.finished());

  REQUIRE(pump_until(fixture.harness, [&turn] { return turn.finished(); }));
  CHECK_FALSE(fixture.conversation.busy());
  CHECK(fixture.conversation.message_count() == 2);
}

TEST_CASE("Harness::cancel addresses an in-flight turn by identifier") {
  auto transport = std::make_unique<HeldTransport>();
  auto* held = transport.get();
  auto harness_result = scry::detail::HarnessTestAccess::create(
      test_config(), provider(), std::move(transport));
  REQUIRE(harness_result);
  auto harness = std::move(*harness_result);
  auto conversation_result = scry::Conversation::create();
  REQUIRE(conversation_result);
  auto conversation = std::move(*conversation_result);

  bool cancelled = false;
  auto turn_result =
      harness.send(conversation, "hold the transfer",
                   {
                       .on_finished =
                           [&cancelled](scry::Result<scry::Completion> outcome) {
                             cancelled = !outcome && outcome.error().category ==
                                                         scry::ErrorCategory::cancelled;
                           },
                   });
  REQUIRE(turn_result);
  const auto turn = std::move(*turn_result);
  held->wait_for_entry();

  CHECK_FALSE(harness.cancel(scry::TurnId{999}));
  CHECK(harness.cancel(turn.id()));
  CHECK_FALSE(harness.cancel(turn.id()));
  REQUIRE(pump_until(harness, [&cancelled] { return cancelled; }));
  CHECK(turn.finished());
  CHECK_FALSE(harness.cancel(turn.id()));
  CHECK(conversation.empty());
  CHECK_FALSE(conversation.busy());
}

TEST_CASE("construction and synchronous admission failures are immediate") {
  auto public_invalid = scry::Harness::create(scry::Config{});
  REQUIRE_FALSE(public_invalid);
  CHECK(public_invalid.error().category == scry::ErrorCategory::invalid_config);

  auto fake = std::make_unique<scry::test::FakeTransport>();
  auto invalid =
      scry::detail::HarnessTestAccess::create(scry::Config{}, nullptr, std::move(fake));
  REQUIRE_FALSE(invalid);
  auto null_provider = scry::detail::HarnessTestAccess::create(
      test_config(), nullptr, std::make_unique<scry::test::FakeTransport>());
  REQUIRE_FALSE(null_provider);
  auto null_transport =
      scry::detail::HarnessTestAccess::create(test_config(), provider(), nullptr);
  REQUIRE_FALSE(null_transport);

  auto config = test_config();
  config.limits.max_conversation_bytes = 1;
  auto harness = fake_harness(config, scripted_exchange(completed_stream));
  auto prompted = scry::Conversation::create({.system_prompt = "too large"});
  auto empty = scry::Conversation::create();
  REQUIRE(harness);
  REQUIRE(prompted);
  REQUIRE(empty);
  CHECK_FALSE(harness->send(*empty, ""));
  CHECK_FALSE(harness->send(*prompted, "x"));
  CHECK_FALSE(harness->send(*empty, "xx"));
  CHECK_FALSE(harness->send_and_wait(*empty, ""));
}

TEST_CASE("rejected admission does not freeze a new tool snapshot generation") {
  auto fixture =
      make_harness_fixture(test_config(), {scripted_exchange(completed_stream)});
  auto& harness = fixture.harness;
  REQUIRE(harness.tools().add(tool(), static_handler(R"({"ok":true})")));
  REQUIRE_FALSE(scry::detail::HarnessTestAccess::has_current_tool_snapshot(harness));

  const auto rejected = harness.send(fixture.conversation, "");
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().category == scry::ErrorCategory::invalid_argument);
  CHECK_FALSE(scry::detail::HarnessTestAccess::has_current_tool_snapshot(harness));

  const auto accepted = harness.send(fixture.conversation, "freeze after validation");
  REQUIRE(accepted);
  CHECK(scry::detail::HarnessTestAccess::has_current_tool_snapshot(harness));
}

TEST_CASE("oversized terminal diagnostics are bounded before publication") {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  fake->enqueue(scry::test::ScriptedExchange{
      .result = std::unexpected(scry::Error{
          .category = scry::ErrorCategory::network,
          .attempt = 9,
          .message = std::string(600, 'm'),
          .provider_detail = std::string(600, 'd'),
          .turn_id = scry::TurnId{.value = 999},
          .provider_request_id = std::string(600, 'r'),
      }),
  });
  auto harness = scry::detail::HarnessTestAccess::create(test_config(), provider(),
                                                         std::move(fake));
  auto conversation = scry::Conversation::create();
  REQUIRE(harness);
  REQUIRE(conversation);

  auto completion = harness->send_and_wait(*conversation, "bound the diagnostic");

  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::network);
  CHECK(completion.error().message ==
        "turn failed; diagnostic exceeded the event buffer");
  CHECK(completion.error().provider_detail.empty());
  CHECK(completion.error().provider_request_id.empty());
  REQUIRE(completion.error().turn_id);
  CHECK(completion.error().turn_id->value != 999);
  CHECK(completion.error().attempt == 1);
}

TEST_CASE("accepted results redact the configured API key from correlation fields") {
  auto config = test_config();
  auto fake = std::make_unique<scry::test::FakeTransport>();
  fake->enqueue({
      .result = std::unexpected(scry::Error{
          .category = scry::ErrorCategory::network,
          .retryable = true,
          .message = "transport echoed sanitized-test-key",
          .provider_detail = "sanitized-test-key",
          .provider_request_id = "request-sanitized-test-key",
      }),
  });
  fake->enqueue(scripted_exchange(completed_stream, config.api_key));
  auto harness =
      scry::detail::HarnessTestAccess::create(config, provider(), std::move(fake));
  auto failed_conversation = scry::Conversation::create();
  auto completed_conversation = scry::Conversation::create();
  REQUIRE(harness);
  REQUIRE(failed_conversation);
  REQUIRE(completed_conversation);

  const auto failure =
      harness->send_and_wait(*failed_conversation, "redact the failure");
  REQUIRE_FALSE(failure);
  CHECK(failure.error().message.find(config.api_key) == std::string::npos);
  CHECK(failure.error().provider_detail.empty());
  CHECK(failure.error().provider_request_id.empty());

  const auto completion =
      harness->send_and_wait(*completed_conversation, "redact the completion");
  REQUIRE(completion);
  CHECK(completion->provider_request_id.empty());
}

TEST_CASE("an oversized completion becomes a bounded queue-limit error") {
  auto config = test_config();
  config.limits.max_queued_event_bytes_per_turn = 1024;
  auto fixture = make_harness_fixture(
      config, {scripted_exchange(completed_stream, std::string(600, 'r'))});

  auto completion =
      fixture.harness.send_and_wait(fixture.conversation, "oversized completion");

  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::resource_limit);
  CHECK(completion.error().message == "turn events exceed the configured queue limit");
  CHECK(fixture.conversation.empty());
}

TEST_CASE("an oversized streamed delta terminates with a queue-limit error") {
  auto config = test_config();
  config.limits.max_queued_event_bytes_per_turn = 1024;
  auto fixture =
      make_harness_fixture(config, {scripted_exchange(large_delta_stream())});

  auto completion =
      fixture.harness.send_and_wait(fixture.conversation, "oversized delta");

  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::resource_limit);
  CHECK(completion.error().message == "turn events exceed the configured queue limit");
  CHECK(fixture.conversation.empty());
}

TEST_CASE("tool content and finish reason must agree before dispatch") {
  auto fixture = make_harness_fixture(test_config(), {scripted_exchange(tool_stream)});

  auto completion =
      fixture.harness.send_and_wait(fixture.conversation, "request a tool");

  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::protocol);
  CHECK(completion.error().message ==
        "tool-use finish reason and tool-call content are inconsistent");
  CHECK(fixture.conversation.empty());
}

TEST_CASE("stream correlation wins over transport correlation") {
  auto fixture = make_harness_fixture(
      test_config(), {scripted_exchange(correlated_stream, "transport-request")});

  auto completion =
      fixture.harness.send_and_wait(fixture.conversation, "preserve correlation");

  REQUIRE(completion);
  CHECK(completion->provider_request_id == "stream-request");
  CHECK(completion->text == "correlated");
}

TEST_CASE("provider request construction failures do not reach transport") {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  auto* observer = fake.get();
  auto harness = scry::detail::HarnessTestAccess::create(
      test_config(), std::make_unique<FailingProvider>(), std::move(fake));
  auto conversation = scry::Conversation::create();
  REQUIRE(harness);
  REQUIRE(conversation);

  auto completion = harness->send_and_wait(*conversation, "fail before transport");

  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::protocol);
  CHECK(completion.error().message == "provider could not construct the request");
  CHECK(observer->requests().empty());
}

TEST_CASE("duplicate provider completions terminate the accepted turn") {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  fake->enqueue(scripted_exchange("data: {}\n\n"));
  auto harness = scry::detail::HarnessTestAccess::create(
      test_config(), std::make_unique<DuplicateCompletionProvider>(), std::move(fake));
  auto conversation = scry::Conversation::create();
  REQUIRE(harness);
  REQUIRE(conversation);

  auto completion = harness->send_and_wait(*conversation, "duplicate completion");

  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::protocol);
  CHECK(completion.error().message ==
        "provider stream emitted more than one completion");
}

TEST_CASE("worker exceptions are contained and reported through the turn") {
  auto harness = scry::detail::HarnessTestAccess::create(
      test_config(), provider(), std::make_unique<ThrowingTransport>());
  auto conversation = scry::Conversation::create();
  REQUIRE(harness);
  REQUIRE(conversation);

  auto completion = harness->send_and_wait(*conversation, "contain exception");

  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::invalid_state);
  CHECK(completion.error().message == "worker could not process the accepted turn");
  CHECK(conversation->empty());
}
