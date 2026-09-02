#include "runtime_test_support.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

TEST_CASE("event byte accounting spans worker queue and pump ownership") {
  scry::detail::EventQueue queue;
  const auto id = scry::TurnId{.value = 8};
  REQUIRE(queue.push(scry::detail::TextDeltaEvent{.turn_id = id, .text = "1234"}, 5));
  auto held_by_pump = queue.try_pop();
  REQUIRE(held_by_pump);

  CHECK_FALSE(queue.push(scry::detail::TextDeltaEvent{.turn_id = id, .text = "56"}, 5));
  queue.release(*held_by_pump);
  CHECK(queue.push(scry::detail::TextDeltaEvent{.turn_id = id, .text = "56"}, 5));
}

TEST_CASE("a turn with empty callbacks still commits its history") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  const auto route = fixture.route(9);
  pump.add_route(route);

  REQUIRE(
      fixture.events->push(completion_event(route->id(), {.text = "answer"}), 1024));
  const auto stats = pump.update({});
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
    scry::detail::PumpState pump{fixture.events};
    std::string received;
    std::size_t callback_count = 0;
    const auto route = fixture.route(
        100 + split,
        {
            .callbacks =
                scry::TurnCallbacks{
                    .on_text_delta =
                        [&received, &callback_count](const std::string_view delta) {
                          received.append(delta);
                          ++callback_count;
                        },
                },
        });
    pump.add_route(route);

    for (std::size_t index = 0; index < split; ++index) {
      REQUIRE(fixture.events->push(
          scry::detail::TextDeltaEvent{
              .turn_id = route->id(),
              .text = std::string{deltas[index]},
          },
          1024));
    }
    static_cast<void>(pump.update({}));
    for (std::size_t index = split; index < deltas.size(); ++index) {
      REQUIRE(fixture.events->push(
          scry::detail::TextDeltaEvent{
              .turn_id = route->id(),
              .text = std::string{deltas[index]},
          },
          1024));
    }
    static_cast<void>(pump.update({}));

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
  scry::detail::PumpState pump{fixture.events};
  const auto route =
      fixture.route(10, {
                            .callbacks =
                                scry::TurnCallbacks{
                                    .on_finished =
                                        [](scry::Result<scry::Completion>) {
                                          throw std::runtime_error{"app callback"};
                                        },
                                },
                        });
  pump.add_route(route);
  REQUIRE(fixture.events->push_terminal(
      scry::detail::ErrorEvent{
          .turn_id = route->id(),
          .error = {.category = scry::ErrorCategory::network},
      },
      1024));

  CHECK_THROWS_AS(pump.update({}), std::runtime_error);
  CHECK(pump.update({}).callbacks_delivered == 0);
}

TEST_CASE("pump budget is a soft deadline between callbacks") {
  PumpFixture fixture;
  auto now = std::chrono::steady_clock::time_point{};
  scry::detail::PumpState pump{
      fixture.events,
      [&now] { return now; },
  };
  const auto first = fixture.route(
      11,
      {
          .callbacks =
              scry::TurnCallbacks{
                  .on_finished = [&now](scry::Result<scry::Completion>) { now += 2ms; },
              },
      });
  const auto second = fixture.route(
      12, {
              .callbacks =
                  scry::TurnCallbacks{
                      .on_finished = [](scry::Result<scry::Completion>) {},
                  },
          });
  pump.add_route(first);
  pump.add_route(second);
  REQUIRE(fixture.events->push(completion_event(first->id(), {.text = "first"}), 1024));
  REQUIRE(
      fixture.events->push(completion_event(second->id(), {.text = "second"}), 1024));

  const auto stats = pump.update({.time_budget = 1ms});
  CHECK(stats.callbacks_delivered == 1);
  CHECK(stats.events_remaining == 1);
  CHECK(stats.budget_exhausted);
  CHECK(pump.update({}).callbacks_delivered == 1);
}

TEST_CASE("pump budget bounds ingestion and delivery after one guaranteed unit") {
  PumpFixture fixture;
  auto now = std::chrono::steady_clock::time_point{};
  scry::detail::PumpState pump{
      fixture.events,
      [&now] {
        const auto sampled = now;
        now += 1ms;
        return sampled;
      },
  };
  bool first_completed = false;
  bool second_completed = false;
  const auto first = fixture.route(
      15, {
              .callbacks =
                  scry::TurnCallbacks{
                      .on_finished =
                          [&first_completed](scry::Result<scry::Completion> done) {
                            first_completed = done.has_value();
                          },
                  },
          });
  const auto second = fixture.route(
      16, {
              .callbacks =
                  scry::TurnCallbacks{
                      .on_finished =
                          [&second_completed](scry::Result<scry::Completion> done) {
                            second_completed = done.has_value();
                          },
                  },
          });
  pump.add_route(first);
  pump.add_route(second);
  REQUIRE(fixture.events->push(completion_event(first->id(), {.text = "first"}), 1024));
  REQUIRE(
      fixture.events->push(completion_event(second->id(), {.text = "second"}), 1024));

  // The clock advances 1 ms per sample. The first pop is unconditional, so it
  // costs no sample; the deadline is then checked before the second pop (at
  // 1 ms, still inside the 2 ms budget) and both events are ingested and
  // committed. The first delivery is likewise unconditional; the check before
  // the second (at 2 ms) stops the call.
  const auto bounded = pump.update({.time_budget = 2ms});
  CHECK(bounded.callbacks_delivered == 1);
  CHECK(bounded.events_remaining == 1);
  CHECK(bounded.budget_exhausted);
  CHECK(fixture.conversation->messages->size() == 4);
  CHECK(first_completed);
  CHECK_FALSE(second_completed);

  const auto drained = pump.update({});
  CHECK(drained.callbacks_delivered == 1);
  CHECK(drained.events_remaining == 0);
  CHECK(second_completed);
  CHECK(fixture.conversation->messages->size() == 4);
}

TEST_CASE("an already-expired positive budget still makes one unit of progress per "
          "call") {
  PumpFixture fixture;
  auto now = std::chrono::steady_clock::time_point{};
  scry::detail::PumpState pump{
      fixture.events,
      [&now] {
        const auto sampled = now;
        now += std::chrono::hours{1};
        return sampled;
      },
  };
  std::size_t completed = 0;
  constexpr std::array turn_values{21U, 22U, 23U};
  for (const auto value : turn_values) {
    const auto route = fixture.route(
        value,
        {
            .callbacks =
                scry::TurnCallbacks{
                    .on_finished =
                        [&completed](scry::Result<scry::Completion>) { ++completed; },
                },
        });
    pump.add_route(route);
    REQUIRE(
        fixture.events->push(completion_event(route->id(), {.text = "done"}), 1024));
  }

  // The budget expires on the very first clock sample, yet each call still
  // ingests one event and delivers one callback.
  const auto first = pump.update({.time_budget = 1us});
  CHECK(first.callbacks_delivered == 1);
  CHECK(first.events_remaining == 2);
  CHECK(first.budget_exhausted);

  const auto second = pump.update({.time_budget = 1us});
  CHECK(second.callbacks_delivered == 1);
  CHECK(second.events_remaining == 1);
  CHECK(second.budget_exhausted);

  const auto third = pump.update({.time_budget = 1us});
  CHECK(third.callbacks_delivered == 1);
  CHECK(third.events_remaining == 0);
  CHECK(completed == 3);
}

TEST_CASE("detaching retains the callbacks supplied at send") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  std::string text;
  const auto route = fixture.route(
      13, {
              .callbacks =
                  scry::TurnCallbacks{
                      .on_text_delta =
                          [&text](const std::string_view delta) { text += delta; },
                  },
          });
  pump.add_route(route);
  route->detach();
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = route->id(), .text = "delta"}, 1024));

  CHECK(pump.update({}).callbacks_delivered == 1);
  CHECK(text == "delta");
}

TEST_CASE("turn cancellation sets the atomic and queues a command once") {
  PumpFixture fixture;
  const auto route = fixture.route(14);
  CHECK(route->cancel());
  CHECK_FALSE(route->cancel());
  CHECK(route->cancel_flag()->load());

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
      std::weak_ptr<scry::detail::CommandQueue>{}, conversation, "question",
      scry::detail::TurnRouteOptions{
          .max_tool_result_bytes = 1024,
          .max_conversation_bytes = 1024,
      });

  CHECK(route->cancel());
  CHECK_FALSE(route->cancel());
  CHECK(cancelled->load(std::memory_order_relaxed));
}

TEST_CASE("a turn route without callbacks claims only its own tool calls") {
  PumpFixture fixture;
  const auto route = fixture.route(212);
  const auto turn_id = route->id();
  CHECK_FALSE(route->has_callback(
      scry::detail::WorkerEvent{scry::detail::TextDeltaEvent{.turn_id = turn_id}}));
  CHECK_FALSE(route->has_callback(scry::detail::WorkerEvent{
      completion_event(turn_id, {.text = "answer", .attempt_count = 2})}));
  CHECK_FALSE(route->has_callback(
      scry::detail::WorkerEvent{scry::detail::ErrorEvent{.turn_id = turn_id}}));
  CHECK_FALSE(route->has_callback(
      scry::detail::WorkerEvent{scry::detail::CancelledEvent{.turn_id = turn_id}}));
  // A tool call is dispatched by the route itself, so it is claimed regardless.
  CHECK(route->has_callback(
      scry::detail::WorkerEvent{scry::detail::ToolCallEvent{.turn_id = turn_id}}));
}

TEST_CASE("a turn route reports completion, failure, and cancellation as one finish") {
  PumpFixture fixture;
  std::string text;
  std::vector<scry::Result<scry::Completion>> finished;
  const auto route = fixture.route(
      213,
      {
          .callbacks = scry::TurnCallbacks{
              .on_text_delta = [&text](const std::string_view value) { text = value; },
              .on_finished =
                  [&finished](scry::Result<scry::Completion> result) {
                    finished.push_back(std::move(result));
                  },
          },
      });
  const scry::detail::WorkerEvent text_event{
      scry::detail::TextDeltaEvent{.turn_id = route->id(), .text = "delta"}};
  const scry::detail::WorkerEvent completed{
      completion_event(route->id(), {.text = "answer", .attempt_count = 2})};
  const scry::detail::WorkerEvent error_event{scry::detail::ErrorEvent{
      .turn_id = route->id(),
      .error = {.category = scry::ErrorCategory::network, .message = "failure"},
  }};
  const scry::detail::WorkerEvent cancelled_event{
      scry::detail::CancelledEvent{.turn_id = route->id()}};
  CHECK(route->has_callback(text_event));
  CHECK(route->has_callback(completed));
  CHECK(route->has_callback(error_event));
  CHECK(route->has_callback(cancelled_event));

  route->invoke(text_event);
  route->invoke(completed);
  route->invoke(error_event);
  route->invoke(cancelled_event);

  CHECK(text == "delta");
  REQUIRE(finished.size() == 3);
  REQUIRE(finished[0]);
  CHECK(finished[0]->attempt_count == 2);
  REQUIRE_FALSE(finished[1]);
  CHECK(finished[1].error().category == scry::ErrorCategory::network);
  CHECK(finished[1].error().message == "failure");
  // Cancellation surfaces as a terminal error carrying the cancelled turn.
  REQUIRE_FALSE(finished[2]);
  CHECK(finished[2].error().category == scry::ErrorCategory::cancelled);
  CHECK(finished[2].error().turn_id == route->id());
}
