#include "runtime/pump.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace {

struct PumpFixture {
  std::shared_ptr<scry::detail::CommandQueue> commands{
      std::make_shared<scry::detail::CommandQueue>()};
  std::shared_ptr<scry::detail::EventQueue> events{
      std::make_shared<scry::detail::EventQueue>()};
  std::shared_ptr<scry::detail::ConversationState> conversation{
      std::make_shared<scry::detail::ConversationState>()};
  std::size_t max_conversation_bytes{1024};

  [[nodiscard]] std::shared_ptr<scry::detail::TurnRoute>
  route(const std::uint64_t id, scry::TurnCallbacks callbacks = {}) const {
    return std::make_shared<scry::detail::TurnRoute>(
        scry::TurnId{.value = id}, std::make_shared<std::atomic<bool>>(false), commands,
        conversation, "q",
        scry::detail::TurnRouteOptions{
            .max_tool_result_bytes = 1024,
            .max_conversation_bytes = max_conversation_bytes,
            .callbacks = std::move(callbacks),
        });
  }
};

[[nodiscard]] scry::detail::CompletionEvent
oversized_completion(const scry::TurnId turn_id) {
  return {
      .turn_id = turn_id,
      .exchange = {scry::detail::Message{
          .role = scry::detail::Role::assistant,
          .content = {scry::detail::TextBlock{
              .text = std::string(128, 'a'),
          }},
      }},
      .finish_reason = scry::detail::FinishReason::completed,
      .attempt_count = 1,
      .provider_request_id = "request-id",
  };
}

} // namespace

TEST_CASE("bounded terminal push preserves the per-turn event byte limit") {
  scry::detail::EventQueue queue;
  const auto turn_id = scry::TurnId{.value = 101};
  constexpr std::size_t limit = 5;

  REQUIRE(queue.push(scry::detail::TextDeltaEvent{.turn_id = turn_id, .text = "1234"},
                     limit));
  CHECK_FALSE(queue.push_terminal(
      scry::detail::ErrorEvent{
          .turn_id = turn_id,
          .error =
              {
                  .category = scry::ErrorCategory::resource_limit,
                  .message = "xy",
              },
      },
      limit));
  CHECK(queue.size() == 1);

  REQUIRE(queue.push_terminal(
      scry::detail::ErrorEvent{
          .turn_id = turn_id,
          .error =
              {
                  .category = scry::ErrorCategory::resource_limit,
                  .message = "x",
              },
      },
      limit));
  CHECK(queue.size() == 2);
  CHECK_FALSE(
      queue.push(scry::detail::TextDeltaEvent{.turn_id = turn_id, .text = "z"}, limit));
}

TEST_CASE("completion mutation releases the originally accounted queue bytes") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  fixture.max_conversation_bytes = 1;
  std::optional<scry::Error> delivered_error;
  const auto route = fixture.route(
      102, scry::TurnCallbacks{
               .on_finished =
                   [&delivered_error](scry::Result<scry::Completion> done) {
                     if (!done) {
                       delivered_error = std::move(done.error());
                     }
                   },
           });
  pump.add_route(route);

  REQUIRE(fixture.events->push(oversized_completion(route->id()), 256));

  const auto stats = pump.update({});
  CHECK(stats.callbacks_delivered == 1);
  REQUIRE(delivered_error);
  CHECK(delivered_error->category == scry::ErrorCategory::resource_limit);
  CHECK(fixture.conversation->messages.empty());

  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{
          .turn_id = route->id(),
          .text = std::string(256, 'b'),
      },
      256));
}

TEST_CASE("events enqueued by a callback wait for the next pump update") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  std::string delivered;
  bool second_enqueue_succeeded = false;
  const auto turn_id = scry::TurnId{.value = 103};
  const auto route = fixture.route(
      turn_id.value,
      scry::TurnCallbacks{
          .on_text_delta =
              [&delivered, &second_enqueue_succeeded, events = fixture.events,
               turn_id](const std::string_view text) {
                delivered.append(text);
                if (text == "first") {
                  second_enqueue_succeeded =
                      events->push(scry::detail::TextDeltaEvent{.turn_id = turn_id,
                                                                .text = "second"},
                                   1024);
                }
              },
      });
  pump.add_route(route);

  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = route->id(), .text = "first"}, 1024));

  const auto first = pump.update({});
  CHECK(second_enqueue_succeeded);
  CHECK(first.callbacks_delivered == 1);
  CHECK(first.events_remaining == 1);
  CHECK(delivered == "first");

  const auto second = pump.update({});
  CHECK(second.callbacks_delivered == 1);
  CHECK(second.events_remaining == 0);
  CHECK(delivered == "firstsecond");
}

TEST_CASE("a rejected reentrant pump update reports an exhausted budget") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  std::optional<scry::UpdateStats> nested;
  bool queued_during_callback = false;
  const auto turn_id = scry::TurnId{.value = 104};
  const auto route = fixture.route(
      turn_id.value,
      scry::TurnCallbacks{
          .on_text_delta =
              [&pump, &nested, &queued_during_callback, events = fixture.events,
               turn_id](const std::string_view text) {
                if (text == "delta") {
                  queued_during_callback = events->push(
                      scry::detail::TextDeltaEvent{.turn_id = turn_id, .text = "later"},
                      1024);
                  nested = pump.update({});
                }
              },
      });
  pump.add_route(route);

  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = route->id(), .text = "delta"}, 1024));

  const auto outer = pump.update({});
  CHECK(queued_during_callback);
  CHECK(outer.callbacks_delivered == 1);
  CHECK(outer.events_remaining == 1);
  CHECK_FALSE(outer.budget_exhausted);
  REQUIRE(nested);
  // The rejection is observable only through budget_exhausted. It delivers
  // nothing and leaves an event queued while the outer pump owns delivery.
  CHECK(nested->budget_exhausted);
  CHECK(nested->callbacks_delivered == 0);
  CHECK(nested->events_remaining == 1);
  CHECK(pump.update({}).callbacks_delivered == 1);
}

TEST_CASE("a nonpositive pump budget expires before queued work") {
  PumpFixture fixture;
  auto now = std::chrono::steady_clock::time_point{};
  scry::detail::PumpState pump{fixture.events, [&now] { return now; }};
  const auto route = fixture.route(105, scry::TurnCallbacks{
                                            .on_text_delta = [](std::string_view) {},
                                        });
  pump.add_route(route);
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = route->id(), .text = "deferred"}, 1024));

  const auto bounded = pump.update({.time_budget = std::chrono::microseconds{0}});
  CHECK(bounded.callbacks_delivered == 0);
  CHECK(bounded.events_remaining == 1);
  CHECK(bounded.budget_exhausted);
  CHECK(pump.update({}).callbacks_delivered == 1);
}
