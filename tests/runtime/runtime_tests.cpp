#include "runtime_test_support.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace std::chrono_literals;
using namespace scry::test_support;

TEST_CASE("event queue coalesces adjacent deltas") {
  scry::detail::EventQueue queue;
  const auto id = scry::TurnId{.value = 7};
  REQUIRE(queue.push(scry::detail::TextDeltaEvent{.turn_id = id, .text = "one"}, 32));
  REQUIRE(queue.push(scry::detail::TextDeltaEvent{.turn_id = id, .text = "two"}, 32));
  REQUIRE(queue.size() == 1);

  auto event = queue.try_pop();
  REQUIRE(event);
  CHECK(std::get<scry::detail::TextDeltaEvent>(*event).text == "onetwo");
  queue.release(*event);
}

TEST_CASE("a turn with empty callbacks still commits its history") {
  PumpFixture fixture;
  const auto route = fixture.add(9);

  REQUIRE(
      fixture.events->push(completion_event(route->id(), {.text = "answer"}), 1024));
  const auto stats = fixture.pump.update({});
  CHECK(stats.callbacks_delivered == 0);
  // No callback can ever consume the completion, so the pump releases its bytes
  // on arrival rather than holding them for a registration that cannot happen.
  CHECK(stats.events_remaining == 0);
  CHECK(fixture.conversation->messages->size() == 2);
  CHECK_FALSE(fixture.conversation->busy);
  CHECK(route->terminal());
}

TEST_CASE("text deltas arrive in order and coalesce within each pump") {
  constexpr auto deltas = std::array<std::string_view, 4>{"one", "-", "two", "-three"};
  for (std::size_t split = 0; split <= deltas.size(); ++split) {
    PumpFixture fixture;
    std::string received;
    std::size_t callback_count = 0;
    const auto route = fixture.add(
        100 + split,
        {
            .callbacks =
                {
                    .on_text_delta =
                        [&received, &callback_count](const std::string_view delta) {
                          received.append(delta);
                          ++callback_count;
                        },
                },
        });

    for (std::size_t index = 0; index < split; ++index) {
      REQUIRE(fixture.events->push(
          scry::detail::TextDeltaEvent{
              .turn_id = route->id(),
              .text = std::string{deltas[index]},
          },
          1024));
    }
    static_cast<void>(fixture.pump.update({}));
    for (std::size_t index = split; index < deltas.size(); ++index) {
      REQUIRE(fixture.events->push(
          scry::detail::TextDeltaEvent{
              .turn_id = route->id(),
              .text = std::string{deltas[index]},
          },
          1024));
    }
    static_cast<void>(fixture.pump.update({}));

    CHECK(received == "one-two-three");
    // Each pump coalesces everything the queue held for the turn into one call,
    // so only a pump that found deltas delivered any.
    const auto expected = static_cast<std::size_t>(split > 0) +
                          static_cast<std::size_t>(split < deltas.size());
    CHECK(callback_count == expected);
  }
}

TEST_CASE("callback exceptions consume the event and leave the pump valid") {
  PumpFixture fixture;
  const auto route =
      fixture.add(10, {
                          .callbacks =
                              {
                                  .on_finished =
                                      [](scry::Result<scry::Completion>) {
                                        throw std::runtime_error{"app callback"};
                                      },
                              },
                      });
  REQUIRE(fixture.events->push(
      scry::detail::ErrorEvent{
          .turn_id = route->id(),
          .error = {.category = scry::ErrorCategory::network},
      },
      1024));

  CHECK_THROWS_AS(fixture.pump.update({}), std::runtime_error);
  CHECK(fixture.pump.update({}).callbacks_delivered == 0);
}

TEST_CASE("pump budget is a soft deadline between callbacks") {
  auto now = std::chrono::steady_clock::time_point{};
  PumpFixture fixture{[&now] { return now; }};
  const auto first = fixture.add(
      11,
      {
          .callbacks =
              {
                  .on_finished = [&now](scry::Result<scry::Completion>) { now += 2ms; },
              },
      });
  const auto second =
      fixture.add(12, {
                          .callbacks =
                              {
                                  .on_finished = [](scry::Result<scry::Completion>) {},
                              },
                      });
  REQUIRE(fixture.events->push(completion_event(first->id(), {.text = "first"}), 1024));
  REQUIRE(
      fixture.events->push(completion_event(second->id(), {.text = "second"}), 1024));

  const auto stats = fixture.pump.update({.time_budget = 1ms});
  CHECK(stats.callbacks_delivered == 1);
  CHECK(stats.events_remaining == 1);
  CHECK(stats.budget_exhausted);
  CHECK(fixture.pump.update({}).callbacks_delivered == 1);
}

TEST_CASE("an already-expired positive budget still makes one unit of progress per "
          "call") {
  auto now = std::chrono::steady_clock::time_point{};
  PumpFixture fixture{[&now] {
    const auto sampled = now;
    now += std::chrono::hours{1};
    return sampled;
  }};
  std::size_t completed = 0;
  constexpr std::array turn_values{21U, 22U, 23U};
  for (const auto value : turn_values) {
    const auto route = fixture.add(
        value,
        {
            .callbacks =
                {
                    .on_finished =
                        [&completed](scry::Result<scry::Completion>) { ++completed; },
                },
        });
    REQUIRE(
        fixture.events->push(completion_event(route->id(), {.text = "done"}), 1024));
  }

  // The budget expires on the very first clock sample, yet each call still
  // ingests one event and delivers one callback.
  const auto first = fixture.pump.update({.time_budget = 1us});
  CHECK(first.callbacks_delivered == 1);
  CHECK(first.events_remaining == 2);
  CHECK(first.budget_exhausted);

  const auto second = fixture.pump.update({.time_budget = 1us});
  CHECK(second.callbacks_delivered == 1);
  CHECK(second.events_remaining == 1);
  CHECK(second.budget_exhausted);

  const auto third = fixture.pump.update({.time_budget = 1us});
  CHECK(third.callbacks_delivered == 1);
  CHECK(third.events_remaining == 0);
  CHECK(completed == 3);
}

TEST_CASE("turn cancellation sets the atomic and queues a command once") {
  PumpFixture fixture;
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  const auto route = std::make_shared<scry::detail::TurnRoute>(
      scry::TurnId{.value = 14}, cancelled, fixture.commands, fixture.conversation,
      scry::detail::TurnRouteOptions{
          .max_tool_result_bytes = 1024,
          .max_conversation_bytes = 1024,
      });
  CHECK(route->cancel());
  CHECK_FALSE(route->cancel());
  CHECK(cancelled->load(std::memory_order_relaxed));

  const auto command = fixture.commands->try_pop();
  REQUIRE(command);
  CHECK(std::get<scry::detail::CancelTurnCommand>(*command).turn_id == route->id());
  CHECK_FALSE(fixture.commands->try_pop());
}

TEST_CASE("turn cancellation stays safe after its command queue expires") {
  auto conversation = std::make_shared<scry::detail::ConversationState>();
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  const auto route = std::make_shared<scry::detail::TurnRoute>(
      scry::TurnId{.value = 221}, cancelled,
      std::weak_ptr<scry::detail::CommandQueue>{}, conversation,
      scry::detail::TurnRouteOptions{
          .max_tool_result_bytes = 1024,
          .max_conversation_bytes = 1024,
      });

  CHECK(route->cancel());
  CHECK_FALSE(route->cancel());
  CHECK(cancelled->load(std::memory_order_relaxed));
}
