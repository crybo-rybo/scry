#include "runtime/test_access.hpp"
#include "support/harness_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace scry::test_support;

namespace {

const std::string anthropic_stream =
    anthropic_text_stream("Hello runtime.", "msg_integration");

[[nodiscard]] scry::test::ScriptedExchange successful_exchange() {
  return scripted_exchange(anthropic_stream, "request-integration");
}

// Blocks inside perform() until the transport is released, so a test can act
// while exactly one transfer is in flight.
[[nodiscard]] scry::test::ScriptedExchange held_exchange() {
  auto exchange = scripted_exchange(anthropic_stream, "controlled-request");
  exchange.hold = true;
  return exchange;
}

} // namespace

TEST_CASE("public async path streams and commits only inside update") {
  auto fixture = make_harness_fixture(test_config(), {successful_exchange()});

  std::string streamed;
  std::string completed;
  std::thread::id callback_thread{};
  auto turn_result = fixture.harness.send(
      fixture.conversation, "Question",
      {
          .on_text_delta =
              [&streamed](const std::string_view text) { streamed.append(text); },
          .on_finished =
              [&completed, &callback_thread](scry::Result<scry::Completion> finished) {
                REQUIRE(finished);
                completed = finished->text;
                callback_thread = std::this_thread::get_id();
              },
      });
  REQUIRE(turn_result);

  CHECK(completed.empty());
  REQUIRE(pump_until(fixture.harness, [&completed] { return !completed.empty(); }));
  CHECK(streamed == "Hello runtime.");
  CHECK(completed == "Hello runtime.");
  CHECK(callback_thread == std::this_thread::get_id());
  CHECK(fixture.conversation.message_count() == 2);
  const auto requests = fixture.transport->requests();
  REQUIRE(requests.size() == 1);
  CHECK(requests.front().body.find("Question") != std::string::npos);
  CHECK(requests.front().body.find("sanitized-test-key") == std::string::npos);
}

TEST_CASE("send-and-wait layers over the async path") {
  auto fixture = make_harness_fixture(test_config(), {successful_exchange()});

  auto completion = fixture.harness.send_and_wait(fixture.conversation, "Question");
  REQUIRE(completion);
  CHECK(completion->text == "Hello runtime.");
  CHECK(completion->finish_reason == scry::FinishReason::completed);
  CHECK(completion->usage.input_tokens == 2);
  CHECK(completion->usage.output_tokens == 2);
  CHECK(completion->attempt_count == 1);
  CHECK(completion->provider_request_id == "request-integration");
  CHECK(fixture.conversation.message_count() == 2);
}

TEST_CASE("accepted failure uses one async error channel and commits nothing") {
  auto config = test_config();
  config.retry.max_attempts = 1;
  auto fixture =
      make_harness_fixture(config, {{
                                       .result = std::unexpected(scry::Error{
                                           .category = scry::ErrorCategory::network,
                                           .retryable = true,
                                           .message = "scripted failure",
                                       }),
                                   }});

  auto completion = fixture.harness.send_and_wait(fixture.conversation, "Question");
  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::network);
  CHECK(completion.error().turn_id.has_value());
  CHECK(completion.error().attempt == 1);
  CHECK(fixture.conversation.empty());
}

TEST_CASE("busy conversations and queued cancellation issue no second transfer") {
  auto config = test_config();
  config.limits.max_pending_turns = 2;
  auto fixture = make_harness_fixture(config, {held_exchange()});
  auto second_conversation = scry::Conversation::create();
  REQUIRE(second_conversation);

  bool first_completed = false;
  bool second_cancelled = false;
  auto first = fixture.harness.send(
      fixture.conversation, "First",
      {
          .on_finished =
              [&first_completed](scry::Result<scry::Completion> finished) {
                first_completed = finished.has_value();
              },
      });
  REQUIRE(first);
  fixture.transport->wait_for_call(1);
  auto busy = fixture.harness.send(fixture.conversation, "Duplicate");
  REQUIRE_FALSE(busy);
  CHECK(busy.error().category == scry::ErrorCategory::busy);
  auto second = fixture.harness.send(
      *second_conversation, "Second",
      {
          .on_finished =
              [&second_cancelled](scry::Result<scry::Completion> finished) {
                second_cancelled = !finished && finished.error().category ==
                                                    scry::ErrorCategory::cancelled;
              },
      });
  REQUIRE(second);
  CHECK(second->cancel());

  fixture.transport->release();
  REQUIRE(
      pump_until(fixture.harness, [&] { return first_completed && second_cancelled; }));
  CHECK(fixture.transport->calls() == 1);
  CHECK(fixture.conversation.message_count() == 2);
  CHECK(second_conversation->empty());
}

TEST_CASE("serialized turns begin in FIFO order with one active transfer") {
  auto config = test_config();
  config.limits.max_pending_turns = 3;
  auto fixture = make_harness_fixture(
      config, {held_exchange(), successful_exchange(), successful_exchange()});
  auto second_conversation = scry::Conversation::create();
  auto third_conversation = scry::Conversation::create();
  REQUIRE(second_conversation);
  REQUIRE(third_conversation);

  std::size_t completed = 0;
  const auto count_completion = [&completed] {
    return scry::TurnCallbacks{
        .on_finished =
            [&completed](scry::Result<scry::Completion> finished) {
              completed += static_cast<std::size_t>(finished.has_value());
            },
    };
  };
  auto first = fixture.harness.send(fixture.conversation, "First FIFO request",
                                    count_completion());
  REQUIRE(first);
  fixture.transport->wait_for_call(1);
  auto second = fixture.harness.send(*second_conversation, "Second FIFO request",
                                     count_completion());
  auto third = fixture.harness.send(*third_conversation, "Third FIFO request",
                                    count_completion());
  REQUIRE(second);
  REQUIRE(third);
  CHECK(fixture.transport->calls() == 1);

  fixture.transport->release();
  REQUIRE(pump_until(fixture.harness, [&completed] { return completed == 3; }));

  const auto requests = fixture.transport->requests();
  REQUIRE(requests.size() == 3);
  CHECK(requests[0].body.find("First FIFO request") != std::string::npos);
  CHECK(requests[1].body.find("Second FIFO request") != std::string::npos);
  CHECK(requests[2].body.find("Third FIFO request") != std::string::npos);
}

TEST_CASE("pending-turn admission limit rejects before acceptance") {
  auto config = test_config();
  config.limits.max_pending_turns = 1;
  auto fixture = make_harness_fixture(config, {held_exchange()});
  auto second_conversation = scry::Conversation::create();
  REQUIRE(second_conversation);

  bool completed = false;
  auto first = fixture.harness.send(
      fixture.conversation, "First",
      {
          .on_finished =
              [&completed](scry::Result<scry::Completion> finished) {
                completed = finished.has_value();
              },
      });
  REQUIRE(first);
  fixture.transport->wait_for_call(1);
  auto rejected = fixture.harness.send(*second_conversation, "Second");
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().category == scry::ErrorCategory::resource_limit);
  fixture.transport->release();
  REQUIRE(pump_until(fixture.harness, [&completed] { return completed; }));
}

TEST_CASE("dropping a Turn detaches without cancelling its callbacks or commit") {
  auto fixture = make_harness_fixture(test_config(), {successful_exchange()});

  bool completed = false;
  {
    auto turn = fixture.harness.send(
        fixture.conversation, "Detached",
        {
            .on_finished =
                [&completed](scry::Result<scry::Completion> finished) {
                  completed = finished.has_value();
                },
        });
    REQUIRE(turn);
  }

  REQUIRE(pump_until(fixture.harness, [&completed] { return completed; }));
  CHECK(fixture.conversation.message_count() == 2);
}

TEST_CASE("two Harness instances keep provider and worker state isolated") {
  auto first = make_harness_fixture(test_config(), {successful_exchange()});
  auto second = make_harness_fixture(test_config(), {successful_exchange()});

  auto first_completion = first.harness.send_and_wait(first.conversation, "First");
  auto second_completion = second.harness.send_and_wait(second.conversation, "Second");
  REQUIRE(first_completion);
  REQUIRE(second_completion);
  CHECK(first.conversation.message_count() == 2);
  CHECK(second.conversation.message_count() == 2);
}
