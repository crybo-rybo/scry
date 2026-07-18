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
  route(const std::uint64_t id) const {
    return std::make_shared<scry::detail::TurnRoute>(
        scry::TurnId{.value = id}, std::make_shared<std::atomic<bool>>(false), commands,
        conversation, "q",
        scry::detail::TurnRouteOptions{
            .max_tool_result_bytes = 1024,
            .max_conversation_bytes = max_conversation_bytes,
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
  const auto route = fixture.route(102);
  pump.add_route(route);

  std::optional<scry::Error> delivered_error;
  REQUIRE(route->register_error(
      [&delivered_error](const scry::Error& error) { delivered_error = error; }));
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
  const auto route = fixture.route(103);
  pump.add_route(route);

  std::string delivered;
  bool second_enqueue_succeeded = false;
  REQUIRE(route->register_text([&delivered, &second_enqueue_succeeded,
                                events = fixture.events,
                                turn_id = route->id()](const std::string_view text) {
    delivered.append(text);
    if (text == "first") {
      second_enqueue_succeeded = events->push(
          scry::detail::TextDeltaEvent{.turn_id = turn_id, .text = "second"}, 1024);
    }
  }));
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

TEST_CASE("reentrant pump update reports an explicit rejection") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  const auto route = fixture.route(104);
  pump.add_route(route);

  std::optional<scry::UpdateStats> nested;
  REQUIRE(route->register_text(
      [&pump, &nested](std::string_view) { nested = pump.update({}); }));
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = route->id(), .text = "delta"}, 1024));

  const auto outer = pump.update({});
  CHECK(outer.callbacks_delivered == 1);
  CHECK_FALSE(outer.reentrant_update_rejected);
  REQUIRE(nested);
  CHECK(nested->reentrant_update_rejected);
  CHECK(nested->budget_exhausted);
}
