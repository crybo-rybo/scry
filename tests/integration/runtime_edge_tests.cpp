#include "support/harness_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <optional>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

using namespace std::chrono_literals;
using namespace scry::test_support;

namespace {

constexpr std::string_view answer = "edge answer";
const std::string completed_stream =
    anthropic_text_stream("edge answer", "msg_edge", {}, 3, 2);

constexpr std::string_view partial_stream = R"(event: message_start
data: {"type":"message_start","message":{"id":"msg_partial","type":"message","role":"assistant","content":[],"model":"test-model","stop_reason":null,"usage":{"input_tokens":3,"output_tokens":0}}}

event: content_block_start
data: {"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}}

event: content_block_delta
data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"partial"}}

)";

// These cases are about retrying, so unlike the shared single-attempt config
// this one leaves the retry budget at its default.
[[nodiscard]] scry::Config retrying_config() {
  auto config = test_config();
  config.retry.max_attempts = scry::RetryPolicy{}.max_attempts;
  return config;
}

[[nodiscard]] ScriptedResponse success() {
  return scripted_response(completed_stream, "request-edge");
}

// A 503 is a retryable network failure.
[[nodiscard]] ScriptedResponse transient_failure() {
  return {.status = 503, .body_chunks = {"transient failure"}};
}

[[nodiscard]] scry::ToolDefinition tool(std::string name = "edge_tool",
                                        std::string schema = R"({"type":"object"})") {
  return {
      .name = std::move(name),
      .description = "edge test tool",
      .input_schema = {.text = std::move(schema)},
  };
}

} // namespace

TEST_CASE("a transient failure retries once and reports the successful attempt") {
  auto fixture =
      make_harness_fixture(retrying_config(), {transient_failure(), success()});

  auto completion = fixture.harness.send_and_wait(fixture.conversation, "retry me");

  REQUIRE(completion);
  CHECK(completion->text == "edge answer");
  CHECK(completion->attempt_count == 2);
  CHECK(completion->provider_request_id == "request-edge");
  CHECK(fixture.server.requests().size() == 2);
  CHECK(fixture.server.remaining() == 0);
  CHECK(fixture.conversation.message_count() == 2);
}

TEST_CASE("semantic output prevents a retry after a transient transport failure") {
  // The connection closes after the partial stream, without the end of the body.
  auto fixture = make_harness_fixture(
      retrying_config(), {
                             {
                                 .body_chunks = {std::string{partial_stream}},
                                 .close_after_chunks = 1,
                             },
                             success(),
                         });

  std::string streamed;
  std::optional<scry::Error> failure_result;
  auto turn = fixture.harness.send(
      fixture.conversation, "do not retry",
      {
          .on_text_delta =
              [&streamed](const std::string_view text) { streamed.append(text); },
          .on_finished =
              [&failure_result](scry::Result<scry::Completion> finished) {
                if (!finished) {
                  failure_result = std::move(finished.error());
                }
              },
      });
  REQUIRE(turn);

  REQUIRE(pump_until(fixture.harness,
                     [&failure_result] { return failure_result.has_value(); }));
  CHECK(streamed == "partial");
  REQUIRE(failure_result);
  CHECK(failure_result->category == scry::ErrorCategory::network);
  CHECK(failure_result->retryable);
  CHECK(failure_result->attempt == 1);
  CHECK(fixture.server.requests().size() == 1);
  CHECK(fixture.server.remaining() == 1);
  CHECK(fixture.conversation.empty());
}

TEST_CASE("cancelling a pending retry wakes the worker without another attempt") {
  auto config = retrying_config();
  config.retry.initial_backoff = 30s;
  config.retry.max_backoff = 30s;
  config.retry.max_elapsed = 60s;
  // One scripted failure only: a second attempt would get the server's
  // unscripted 404 and be counted below.
  auto fixture = make_harness_fixture(config, {transient_failure()});

  bool cancelled = false;
  auto turn = fixture.harness.send(
      fixture.conversation, "cancel retry",
      {
          .on_finished =
              [&cancelled](scry::Result<scry::Completion> finished) {
                cancelled = !finished &&
                            finished.error().category == scry::ErrorCategory::cancelled;
              },
      });
  REQUIRE(turn);
  REQUIRE(fixture.server.wait_for_request(1));

  // Without the wake, the 30 s backoff outlasts pump_until's deadline.
  CHECK(turn->cancel());
  REQUIRE(pump_until(fixture.harness, [&cancelled] { return cancelled; }));
  CHECK_FALSE(turn->cancel());
  CHECK(fixture.server.requests().size() == 1);
  CHECK(fixture.conversation.empty());
}

TEST_CASE("a queued turn command is consumed before a zero-backoff retry wakes") {
  auto config = test_config();
  config.retry.max_attempts = 2;
  config.retry.initial_backoff = 0ms;
  config.retry.max_backoff = 0ms;
  // The first attempt is held so the second turn's command is already queued
  // when the zero-backoff retry wait evaluates its predicate; the retry and the
  // second turn then consume the two successes.
  auto held_failure = transient_failure();
  held_failure.hold = true;
  auto fixture = make_harness_fixture(config, {held_failure, success(), success()});
  auto second_conversation = unwrap(scry::Conversation::create());

  std::optional<scry::Completion> first_completion;
  std::optional<scry::Error> first_failure;
  auto first_turn =
      fixture.harness.send(fixture.conversation, "retry after queued command",
                           {
                               .on_finished =
                                   [&first_completion, &first_failure](
                                       scry::Result<scry::Completion> finished) {
                                     if (finished) {
                                       first_completion = std::move(*finished);
                                     } else {
                                       first_failure = std::move(finished.error());
                                     }
                                   },
                           });
  REQUIRE(first_turn);
  REQUIRE(fixture.server.wait_for_request(1));

  std::optional<scry::Completion> second_completion;
  std::optional<scry::Error> second_failure;
  auto second_turn =
      fixture.harness.send(second_conversation, "queued while retrying",
                           {
                               .on_finished =
                                   [&second_completion, &second_failure](
                                       scry::Result<scry::Completion> finished) {
                                     if (finished) {
                                       second_completion = std::move(*finished);
                                     } else {
                                       second_failure = std::move(finished.error());
                                     }
                                   },
                           });
  REQUIRE(second_turn);

  // The worker is still inside the held transfer, so the second SendTurnCommand
  // is queued when the zero-backoff retry wait evaluates its predicate.
  fixture.server.release();

  REQUIRE(pump_until(fixture.harness, [&] {
    return (first_completion || first_failure) && (second_completion || second_failure);
  }));
  REQUIRE_FALSE(first_failure);
  REQUIRE_FALSE(second_failure);
  REQUIRE(first_completion);
  REQUIRE(second_completion);
  CHECK(first_completion->attempt_count == 2);
  CHECK(second_completion->attempt_count == 1);
  CHECK(fixture.server.requests().size() == 3);
  CHECK(fixture.conversation.message_count() == 2);
  CHECK(second_conversation.message_count() == 2);
}

TEST_CASE("a completion one byte over the Conversation limit is not committed") {
  constexpr std::string_view question = "limit";
  auto config = retrying_config();
  config.limits.max_conversation_bytes = question.size() + answer.size() - 1;
  auto fixture = make_harness_fixture(config, {success()});

  auto completion =
      fixture.harness.send_and_wait(fixture.conversation, std::string{question});

  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::resource_limit);
  CHECK(completion.error().attempt == 1);
  CHECK(completion.error().provider_request_id == "request-edge");
  CHECK(fixture.conversation.empty());
  CHECK(fixture.conversation.message_count() == 0);
}

TEST_CASE("post-completion cancellation is safe and idempotent") {
  auto fixture = make_harness_fixture(retrying_config(), {success()});

  bool completed = false;
  std::size_t finished_count = 0;
  auto turn = fixture.harness.send(
      fixture.conversation, "complete first",
      {
          .on_finished =
              [&completed, &finished_count](scry::Result<scry::Completion> finished) {
                completed = finished.has_value();
                ++finished_count;
              },
      });
  REQUIRE(turn);
  REQUIRE(pump_until(fixture.harness, [&completed] { return completed; }));

  CHECK_FALSE(turn->cancel());
  for (std::size_t pump = 0; pump < 32; ++pump) {
    static_cast<void>(fixture.harness.update());
    std::this_thread::yield();
  }
  // The terminal contract is exactly once: a cancel after completion delivers
  // nothing further.
  CHECK(finished_count == 1);
  CHECK(fixture.conversation.message_count() == 2);
}

TEST_CASE("callbacks may use public operations and nested update is diagnosed") {
  auto fixture = make_harness_fixture(retrying_config(), {success(), success()});
  auto& harness = fixture.harness;
  auto& first_conversation = fixture.conversation;
  auto second_conversation = unwrap(scry::Conversation::create());

  std::optional<scry::Turn> first;
  std::optional<scry::Turn> second;
  bool first_completed = false;
  bool second_completed = false;
  bool nested_update_rejected = false;
  bool registration_succeeded = false;
  bool nested_send_succeeded = false;
  bool terminal_cancel_idempotent = false;
  std::optional<scry::Error> nested_wait_error;
  auto first_result = harness.send(
      first_conversation, "first",
      {
          .on_finished =
              [&](scry::Result<scry::Completion> finished) {
                first_completed = finished.has_value();
                // A reentrant update is rejected, and budget_exhausted is the
                // only signal of it.
                nested_update_rejected = harness.update().budget_exhausted;
                registration_succeeded = static_cast<bool>(harness.tools().add_dynamic(
                    tool(), static_handler(R"({"ok":true})")));
                auto nested_wait =
                    harness.send_and_wait(second_conversation, "blocking second");
                if (!nested_wait) {
                  nested_wait_error = nested_wait.error();
                }
                terminal_cancel_idempotent = !first->cancel();
                auto nested = harness.send(
                    second_conversation, "second",
                    {
                        .on_finished =
                            [&second_completed](scry::Result<scry::Completion> done) {
                              second_completed = done.has_value();
                            },
                    });
                nested_send_succeeded = nested.has_value();
                if (nested) {
                  second.emplace(std::move(*nested));
                }
              },
      });
  REQUIRE(first_result);
  first.emplace(std::move(*first_result));

  REQUIRE(pump_until(harness, [&] { return first_completed && second_completed; }));
  CHECK(nested_update_rejected);
  CHECK(registration_succeeded);
  REQUIRE(nested_wait_error);
  CHECK(nested_wait_error->category == scry::ErrorCategory::invalid_state);
  CHECK(terminal_cancel_idempotent);
  CHECK(nested_send_succeeded);
  CHECK(harness.tools().size() == 1);
  CHECK(first_conversation.message_count() == 2);
  CHECK(second_conversation.message_count() == 2);
}

TEST_CASE("two Harness workers can overlap independent transfers") {
  auto held = success();
  held.hold = true;
  auto first = make_harness_fixture(retrying_config(), {held});
  auto second = make_harness_fixture(retrying_config(), {held});

  bool first_completed = false;
  bool second_completed = false;
  auto first_turn = first.harness.send(
      first.conversation, "first",
      {
          .on_finished =
              [&first_completed](scry::Result<scry::Completion> finished) {
                first_completed = finished.has_value();
              },
      });
  auto second_turn = second.harness.send(
      second.conversation, "second",
      {
          .on_finished =
              [&second_completed](scry::Result<scry::Completion> finished) {
                second_completed = finished.has_value();
              },
      });
  REQUIRE(first_turn);
  REQUIRE(second_turn);

  // Both servers hold their response until both requests have arrived, so
  // both workers are inside a transfer at once.
  REQUIRE(first.server.wait_for_request(1));
  REQUIRE(second.server.wait_for_request(1));
  first.server.release();
  second.server.release();

  REQUIRE(pump_until(first.harness, [&first_completed] { return first_completed; }));
  REQUIRE(pump_until(second.harness, [&second_completed] { return second_completed; }));
  CHECK(first.conversation.message_count() == 2);
  CHECK(second.conversation.message_count() == 2);
}
