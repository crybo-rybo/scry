#include "runtime/pump.hpp"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace std::chrono_literals;

namespace {

struct RouteFixture {
  std::shared_ptr<scry::detail::CommandQueue> commands{
      std::make_shared<scry::detail::CommandQueue>()};
  std::shared_ptr<scry::detail::EventQueue> events{
      std::make_shared<scry::detail::EventQueue>()};
  std::shared_ptr<scry::detail::ConversationState> conversation{
      std::make_shared<scry::detail::ConversationState>()};

  [[nodiscard]] std::shared_ptr<scry::detail::TurnRoute>
  route(const std::uint64_t id, scry::TurnCallbacks callbacks = {},
        std::string user = "question") const {
    return std::make_shared<scry::detail::TurnRoute>(
        scry::TurnId{.value = id}, std::make_shared<std::atomic<bool>>(false), commands,
        conversation, std::move(user),
        scry::detail::TurnRouteOptions{
            .max_tool_result_bytes = 1024,
            .max_conversation_bytes = 1024,
            .callbacks = std::move(callbacks),
        });
  }
};

[[nodiscard]] scry::detail::CompletionEvent completion(const std::uint64_t id,
                                                       std::string text) {
  return scry::detail::CompletionEvent{
      .turn_id = scry::TurnId{.value = id},
      .exchange = {scry::detail::Message{
          .role = scry::detail::Role::assistant,
          .content = {scry::detail::TextBlock{.text = std::move(text)}},
      }},
      .finish_reason = scry::detail::FinishReason::completed,
      .attempt_count = 1,
      .provider_request_id = "request-id",
  };
}

} // namespace

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
  RouteFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  const auto route = fixture.route(9);
  pump.add_route(route);

  REQUIRE(fixture.events->push(completion(9, "answer"), 1024));
  const auto stats = pump.update({});
  CHECK(stats.callbacks_delivered == 0);
  // No callback can ever consume the completion, so the pump releases its bytes
  // on arrival rather than holding them for a registration that cannot happen.
  CHECK(stats.events_remaining == 0);
  CHECK(fixture.conversation->messages.size() == 2);
  CHECK_FALSE(fixture.conversation->busy);
  CHECK(route->terminal());
}

TEST_CASE("text deltas arrive in order and coalesce within each pump") {
  constexpr auto deltas = std::array<std::string_view, 4>{"one", "-", "two", "-three"};
  for (std::size_t split = 0; split <= deltas.size(); ++split) {
    RouteFixture fixture;
    scry::detail::PumpState pump{fixture.events};
    std::string received;
    std::size_t callback_count = 0;
    const auto route = fixture.route(
        100 + split,
        scry::TurnCallbacks{
            .on_text_delta =
                [&received, &callback_count](const std::string_view delta) {
                  received.append(delta);
                  ++callback_count;
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
  RouteFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  const auto route =
      fixture.route(10, scry::TurnCallbacks{
                            .on_finished =
                                [](scry::Result<scry::Completion>) {
                                  throw std::runtime_error{"app callback"};
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
  RouteFixture fixture;
  auto now = std::chrono::steady_clock::time_point{};
  scry::detail::PumpState pump{
      fixture.events,
      [&now] { return now; },
  };
  const auto first = fixture.route(
      11, scry::TurnCallbacks{
              .on_finished = [&now](scry::Result<scry::Completion>) { now += 2ms; },
          });
  const auto second =
      fixture.route(12, scry::TurnCallbacks{
                            .on_finished = [](scry::Result<scry::Completion>) {},
                        });
  pump.add_route(first);
  pump.add_route(second);
  REQUIRE(fixture.events->push(completion(11, "first"), 1024));
  REQUIRE(fixture.events->push(completion(12, "second"), 1024));

  const auto stats = pump.update({.time_budget = 1ms});
  CHECK(stats.callbacks_delivered == 1);
  CHECK(stats.events_remaining == 1);
  CHECK(stats.budget_exhausted);
  CHECK(pump.update({}).callbacks_delivered == 1);
}

TEST_CASE("pump budget bounds event ingestion and terminal commits") {
  RouteFixture fixture;
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
      15, scry::TurnCallbacks{
              .on_finished =
                  [&first_completed](scry::Result<scry::Completion> done) {
                    first_completed = done.has_value();
                  },
          });
  const auto second = fixture.route(
      16, scry::TurnCallbacks{
              .on_finished =
                  [&second_completed](scry::Result<scry::Completion> done) {
                    second_completed = done.has_value();
                  },
          });
  pump.add_route(first);
  pump.add_route(second);
  REQUIRE(fixture.events->push(completion(15, "first"), 1024));
  REQUIRE(fixture.events->push(completion(16, "second"), 1024));

  const auto bounded = pump.update({.time_budget = 2ms});
  CHECK(bounded.callbacks_delivered == 0);
  CHECK(bounded.events_remaining == 2);
  CHECK(bounded.budget_exhausted);
  CHECK(fixture.conversation->messages.size() == 2);
  CHECK_FALSE(first_completed);
  CHECK_FALSE(second_completed);

  const auto drained = pump.update({});
  CHECK(drained.callbacks_delivered == 2);
  CHECK(drained.events_remaining == 0);
  CHECK(first_completed);
  CHECK(second_completed);
  CHECK(fixture.conversation->messages.size() == 4);
}

TEST_CASE("detaching retains the callbacks supplied at send") {
  RouteFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  std::string text;
  const auto route = fixture.route(
      13, scry::TurnCallbacks{
              .on_text_delta = [&text](const std::string_view delta) { text += delta; },
          });
  pump.add_route(route);
  route->detach();
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = route->id(), .text = "delta"}, 1024));

  CHECK(pump.update({}).callbacks_delivered == 1);
  CHECK(text == "delta");
}

TEST_CASE("turn cancellation sets the atomic and queues a command once") {
  RouteFixture fixture;
  const auto route = fixture.route(14);
  CHECK(route->cancel());
  CHECK_FALSE(route->cancel());
  CHECK(route->cancel_flag()->load());

  const auto command = fixture.commands->try_pop();
  REQUIRE(command);
  CHECK(std::get<scry::detail::CancelTurnCommand>(*command).turn_id == route->id());
  CHECK_FALSE(fixture.commands->try_pop());
}
