#include "support/harness_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <optional>
#include <scry/scry.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace scry::test_support;

namespace {

const std::string completed_stream =
    anthropic_text_stream("coverage answer", "msg_coverage", {}, 3, 2);

const std::string correlated_stream =
    anthropic_text_stream("correlated", "msg_correlated", "stream-request", 3, 1);

// Announces a tool call and then ends the turn: content and finish reason
// disagree, which is what the test using it asserts on.
const std::string tool_stream =
    anthropic_tool_stream({{.id = "call_1", .name = "lookup"}}, "msg_tool", "end_turn");

// A single text delta larger than the part of the test's tightened event-queue
// budget left for streamed events once the terminal reserve is held back.
[[nodiscard]] std::string large_delta_stream() {
  return anthropic_text_stream(std::string(600, 'x'), "msg_large", {}, 1, 1);
}

[[nodiscard]] scry::ToolDefinition tool() {
  return {
      .name = "coverage_tool",
      .description = "runtime coverage tool",
      .input_schema = {.text = R"({"type":"object"})"},
  };
}

} // namespace

TEST_CASE("moved-from public runtime handles remain safely observable") {
  auto server = start_server();
  server.enqueue(scripted_response(completed_stream));
  auto config = test_config();
  config.base_url = server.url();
  auto harness_result = scry::Harness::create(config);
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
  REQUIRE(harness.tools().add_dynamic(tool(), static_handler(R"({"ok":true})")));
  CHECK(const_tools.size() == 1);

  auto turn_result = harness.send(conversation, "question");
  REQUIRE(turn_result);
  auto turn = std::move(*turn_result);
  CHECK_FALSE(turn_result->id());
  CHECK_FALSE(turn_result->cancel());
  CHECK_FALSE(turn_result->disconnect());
  CHECK(turn.id());
}

TEST_CASE("a Turn does not cancel after its Harness has been destroyed") {
  std::optional<scry::Turn> survivor;
  scry::TurnId accepted_id{};
  {
    auto fixture =
        make_harness_fixture(test_config(), {scripted_response(completed_stream)});
    auto turn = fixture.harness.send(fixture.conversation, "outlive the harness");
    REQUIRE(turn);
    accepted_id = turn->id();
    survivor.emplace(std::move(*turn));
  }

  REQUIRE(survivor);
  CHECK(survivor->id() == accepted_id);
  CHECK(survivor->finished());
  CHECK_FALSE(survivor->cancel());
  CHECK_FALSE(survivor->cancel());
}

TEST_CASE("a completed turn publishes its history, busy state, and finished flag") {
  auto fixture =
      make_harness_fixture(test_config(), {scripted_response(completed_stream)});
  auto& harness = fixture.harness;
  auto conversation =
      unwrap(scry::Conversation::create({.system_prompt = "Be brief."}));
  CHECK(conversation.system_prompt() == "Be brief.");
  CHECK_FALSE(conversation.busy());

  bool finished = false;
  const auto turn =
      unwrap(harness.send(conversation, "question",
                          {
                              .on_finished =
                                  [&finished](scry::Result<scry::Completion> outcome) {
                                    finished = outcome.has_value();
                                  },
                          }));
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
      make_harness_fixture(test_config(), {scripted_response(completed_stream)});

  const auto turn = unwrap(fixture.harness.send(fixture.conversation, "question"));
  CHECK_FALSE(turn.finished());

  REQUIRE(pump_until(fixture.harness, [&turn] { return turn.finished(); }));
  CHECK_FALSE(fixture.conversation.busy());
  CHECK(fixture.conversation.message_count() == 2);
}

TEST_CASE("Harness::cancel addresses an in-flight turn by identifier") {
  // A held response keeps the transfer in flight until the turn is cancelled.
  auto [server, harness, conversation] =
      make_harness_fixture(test_config(), {{.hold = true}});

  bool cancelled = false;
  const auto turn =
      unwrap(harness.send(conversation, "hold the transfer",
                          {
                              .on_finished =
                                  [&cancelled](scry::Result<scry::Completion> outcome) {
                                    cancelled =
                                        !outcome && outcome.error().category ==
                                                        scry::ErrorCategory::cancelled;
                                  },
                          }));
  REQUIRE(server.wait_for_request(1));

  CHECK_FALSE(harness.cancel(scry::TurnId{999}));
  CHECK(harness.cancel(turn.id()));
  CHECK_FALSE(harness.cancel(turn.id()));
  REQUIRE(pump_until(harness, [&cancelled] { return cancelled; }));
  CHECK(turn.finished());
  CHECK_FALSE(harness.cancel(turn.id()));
  CHECK(conversation.empty());
  CHECK_FALSE(conversation.busy());
}

TEST_CASE("Harness::disconnect stops delivery while the turn still runs") {
  // A held response keeps the transfer in flight until the turn is cancelled.
  auto [server, harness, conversation] =
      make_harness_fixture(test_config(), {{.hold = true}});

  std::string streamed;
  bool reported = false;
  const auto turn = unwrap(harness.send(
      conversation, "hold the transfer",
      {
          .on_text_delta =
              [&streamed](std::string_view delta) { streamed.append(delta); },
          .on_finished =
              [&reported](scry::Result<scry::Completion>) { reported = true; },
      }));
  REQUIRE(server.wait_for_request(1));

  CHECK_FALSE(harness.disconnect(scry::TurnId{999}));
  CHECK(harness.disconnect(turn.id()));
  CHECK_FALSE(harness.disconnect(turn.id()));
  // Disconnection stopped the reporting; cancellation still stops the work,
  // and the two compose in either order.
  CHECK(harness.cancel(turn.id()));

  REQUIRE(pump_until(harness, [&turn] { return turn.finished(); }));
  CHECK_FALSE(reported);
  CHECK(streamed.empty());
  CHECK(conversation.empty());
  CHECK_FALSE(conversation.busy());
  CHECK_FALSE(harness.disconnect(turn.id()));
}

TEST_CASE("construction and synchronous admission failures are immediate") {
  auto public_invalid = scry::Harness::create(scry::Config{});
  REQUIRE_FALSE(public_invalid);
  CHECK(public_invalid.error().category == scry::ErrorCategory::invalid_config);

  // Admission fails before any transfer, so this Harness needs no server.
  auto config = test_config();
  config.limits.max_conversation_bytes = 1;
  auto harness = scry::Harness::create(config);
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

TEST_CASE("a rejected send leaves the conversation and tool registry reusable") {
  auto fixture =
      make_harness_fixture(test_config(), {scripted_response(completed_stream)});
  auto& harness = fixture.harness;
  REQUIRE(harness.tools().add_dynamic(tool(), static_handler(R"({"ok":true})")));

  const auto rejected = harness.send(fixture.conversation, "");
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().category == scry::ErrorCategory::invalid_argument);
  CHECK(fixture.conversation.empty());
  CHECK_FALSE(fixture.conversation.busy());
  CHECK(fixture.server.requests().empty());

  auto added_after_rejection = tool();
  added_after_rejection.name = "added_after_rejection";
  REQUIRE(harness.tools().add_dynamic(std::move(added_after_rejection),
                                      static_handler(R"({"ok":true})")));

  const auto completion =
      harness.send_and_wait(fixture.conversation, "send after validation failure");
  REQUIRE(completion);
  CHECK(completion->text == "coverage answer");
  CHECK(fixture.conversation.message_count() == 2);
  CHECK_FALSE(fixture.conversation.busy());

  const auto requests = fixture.server.requests();
  REQUIRE(requests.size() == 1);
  CHECK(requests.front().body.find("coverage_tool") != std::string::npos);
  CHECK(requests.front().body.find("added_after_rejection") != std::string::npos);
}

// An Anthropic in-stream error event can carry a request id of any size, so a
// real server can make the terminal error larger than the worker's reserve.
TEST_CASE("oversized terminal diagnostics are bounded before publication") {
  const auto error_stream =
      "event: error\ndata: "
      R"({"type":"error","error":{"type":"overloaded_error","message":"busy"},)"
      R"("request_id":")" +
      std::string(600, 'r') + "\"}\n\n";
  auto fixture = make_harness_fixture(test_config(), {scripted_response(error_stream)});

  auto completion =
      fixture.harness.send_and_wait(fixture.conversation, "bound the diagnostic");

  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::network);
  CHECK(completion.error().message ==
        "turn failed; diagnostic exceeded the event buffer");
  CHECK(completion.error().provider_detail.empty());
  CHECK(completion.error().provider_request_id.empty());
  CHECK(completion.error().turn_id);
  CHECK(completion.error().attempt == 1);
}

TEST_CASE("accepted results redact the configured API key from correlation fields") {
  // A key that the provider-detail sanitizer keeps, so an error body can echo it.
  auto config = test_config();
  config.api_key = "scripted_secret_key";
  auto fixture = make_harness_fixture(
      config, {
                  {
                      .status = 500,
                      .headers = {{.name = "request-id",
                                   .value = "request-scripted_secret_key"}},
                      .body_chunks = {anthropic_error_body("scripted_secret_key")},
                  },
                  scripted_response(completed_stream, config.api_key),
              });
  auto completed_conversation = unwrap(scry::Conversation::create());

  const auto failure =
      fixture.harness.send_and_wait(fixture.conversation, "redact the failure");
  REQUIRE_FALSE(failure);
  CHECK(failure.error().http_status == 500);
  CHECK(failure.error().message.find(config.api_key) == std::string::npos);
  CHECK(failure.error().provider_detail.empty());
  CHECK(failure.error().provider_request_id.empty());

  const auto completion =
      fixture.harness.send_and_wait(completed_conversation, "redact the completion");
  REQUIRE(completion);
  CHECK(completion->provider_request_id.empty());
}

// The exchange a completion carries is reserved against the Conversation budget
// by the machine, so the queue charges the completion only its correlation id.
// Transport policy caps a header id at 256 bytes, well inside the terminal
// reserve. An id from the stream has no such cap, and one past the reserve is
// dropped rather than failing the turn.
TEST_CASE("a completion is never charged against the queue limit") {
  auto config = test_config();
  config.limits.max_queued_event_bytes_per_turn = 1024;

  SECTION("a correlation id within the terminal reserve is delivered intact") {
    auto fixture = make_harness_fixture(
        config, {scripted_response(completed_stream, std::string(256, 'r'))});

    auto completion =
        fixture.harness.send_and_wait(fixture.conversation, "correlated completion");

    REQUIRE(completion);
    CHECK(completion->provider_request_id.size() == 256);
    CHECK(fixture.conversation.message_count() == 2);
  }

  SECTION("a correlation id past the terminal reserve is dropped, not failed") {
    auto fixture = make_harness_fixture(
        config, {scripted_response(anthropic_text_stream(
                    "coverage answer", "msg_large_id", std::string(600, 'r')))});

    auto completion =
        fixture.harness.send_and_wait(fixture.conversation, "oversized completion");

    REQUIRE(completion);
    CHECK(completion->text == "coverage answer");
    CHECK(completion->provider_request_id.empty());
    CHECK(fixture.conversation.message_count() == 2);
  }
}

TEST_CASE("an oversized streamed delta terminates with a queue-limit error") {
  auto config = test_config();
  config.limits.max_queued_event_bytes_per_turn = 1024;
  auto fixture =
      make_harness_fixture(config, {scripted_response(large_delta_stream())});

  auto completion =
      fixture.harness.send_and_wait(fixture.conversation, "oversized delta");

  // The queue rejects the delta inside the transport's response sink, and the
  // transport reports a sink failure with a fixed message of its own.
  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::resource_limit);
  CHECK(completion.error().message == "response consumer rejected response data");
  CHECK(fixture.conversation.empty());
}

TEST_CASE("tool content and finish reason must agree before dispatch") {
  auto fixture = make_harness_fixture(test_config(), {scripted_response(tool_stream)});

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
      test_config(), {scripted_response(correlated_stream, "transport-request")});

  auto completion =
      fixture.harness.send_and_wait(fixture.conversation, "preserve correlation");

  REQUIRE(completion);
  CHECK(completion->provider_request_id == "stream-request");
  CHECK(completion->text == "correlated");
}

TEST_CASE("send_and_wait disconnects its turn when another turn's callback throws") {
  auto fixture = make_harness_fixture(
      test_config(), {scripted_response(anthropic_text_stream("first")),
                      scripted_response(anthropic_text_stream("second"))});
  // The throwing turn is accepted first, so its delta is delivered inside the
  // wait below and unwinds send_and_wait before its own turn terminates.
  auto first = unwrap(fixture.harness.send(
      fixture.conversation, "first",
      {
          .on_text_delta =
              [](std::string_view) { throw std::runtime_error("host callback"); },
      }));
  auto second = unwrap(scry::Conversation::create());

  REQUIRE_THROWS_AS(fixture.harness.send_and_wait(second, "second"),
                    std::runtime_error);

  // The abandoned turn keeps running and commits, but nothing reaches the stack
  // frame that send_and_wait left behind.
  REQUIRE(pump_until(fixture.harness, [&second] { return !second.busy(); }));
  CHECK(second.message_count() == 2);
}

// A retained Turn handle keeps identity and status, not the host state the
// runtime needed while the turn was running.
TEST_CASE("a finished turn releases its callback captures while its handle lives") {
  auto fixture =
      make_harness_fixture(test_config(), {scripted_response(completed_stream)});
  auto captured = std::make_shared<int>(5);
  const std::weak_ptr<int> observed = captured;
  bool finished = false;

  auto turn = unwrap(fixture.harness.send(
      fixture.conversation, "release the captures",
      {
          .on_finished = [&finished, held = std::move(captured)](
                             scry::Result<scry::Completion>) { finished = *held == 5; },
      }));

  REQUIRE(pump_until(fixture.harness, [&turn] { return turn.finished(); }));
  CHECK(finished);
  CHECK(turn.finished());
  CHECK(observed.expired());
}
