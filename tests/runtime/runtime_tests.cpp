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
  route(const std::uint64_t id, std::string user = "question") const {
    return std::make_shared<scry::detail::TurnRoute>(
        scry::TurnId{.value = id}, std::make_shared<std::atomic<bool>>(false), commands,
        conversation, std::move(user), 1024);
  }
};

[[nodiscard]] scry::detail::CompletionEvent completion(const std::uint64_t id,
                                                       std::string text) {
  return scry::detail::CompletionEvent{
      .turn_id = scry::TurnId{.value = id},
      .response =
          scry::detail::ModelResponse{
              .content = {scry::detail::TextBlock{.text = std::move(text)}},
              .finish_reason = scry::detail::FinishReason::completed,
              .provider_request_id = "request-id",
          },
      .attempt_count = 1,
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

TEST_CASE("pump commits completion before a late callback is registered") {
  RouteFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  const auto route = fixture.route(9);
  pump.add_route(route);

  REQUIRE(fixture.events->push(completion(9, "answer"), 1024));
  const auto first = pump.update({});
  CHECK(first.callbacks_delivered == 0);
  CHECK(first.events_remaining == 1);
  CHECK(fixture.conversation->messages.size() == 2);
  CHECK_FALSE(fixture.conversation->busy);

  std::string answer;
  REQUIRE(route->register_completion(
      [&answer](const scry::Completion& result) { answer = result.text; }));
  const auto second = pump.update({});
  CHECK(second.callbacks_delivered == 1);
  CHECK(second.events_remaining == 0);
  CHECK(answer == "answer");
}

TEST_CASE("late text callback receives every buffered delta in order") {
  constexpr auto deltas = std::array<std::string_view, 4>{"one", "-", "two", "-three"};
  for (std::size_t split = 0; split <= deltas.size(); ++split) {
    RouteFixture fixture;
    scry::detail::PumpState pump{fixture.events};
    const auto route = fixture.route(100 + split);
    pump.add_route(route);

    for (std::size_t index = 0; index < split; ++index) {
      REQUIRE(fixture.events->push(
          scry::detail::TextDeltaEvent{
              .turn_id = route->id(),
              .text = std::string{deltas[index]},
          },
          1024));
    }
    CHECK(pump.update({}).callbacks_delivered == 0);

    std::string received;
    std::size_t callback_count = 0;
    REQUIRE(route->register_text(
        [&received, &callback_count](const std::string_view delta) {
          received.append(delta);
          ++callback_count;
        }));
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
    CHECK(callback_count == 1);
  }
}

TEST_CASE("callback exceptions consume the event and leave the pump valid") {
  RouteFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  const auto route = fixture.route(10);
  pump.add_route(route);
  REQUIRE(route->register_error(
      [](const scry::Error&) { throw std::runtime_error{"app callback"}; }));
  fixture.events->push_terminal(scry::detail::ErrorEvent{
      .turn_id = route->id(),
      .error = {.category = scry::ErrorCategory::network},
  });

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
  const auto first = fixture.route(11);
  const auto second = fixture.route(12);
  pump.add_route(first);
  pump.add_route(second);
  REQUIRE(first->register_completion([&now](const scry::Completion&) { now += 2ms; }));
  REQUIRE(second->register_completion([](const scry::Completion&) {}));
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
  const auto first = fixture.route(15);
  const auto second = fixture.route(16);
  pump.add_route(first);
  pump.add_route(second);
  bool first_completed = false;
  bool second_completed = false;
  REQUIRE(first->register_completion(
      [&first_completed](const scry::Completion&) { first_completed = true; }));
  REQUIRE(second->register_completion(
      [&second_completed](const scry::Completion&) { second_completed = true; }));
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

TEST_CASE("detaching retains callbacks already registered") {
  RouteFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  const auto route = fixture.route(13);
  pump.add_route(route);
  std::string text;
  REQUIRE(
      route->register_text([&text](const std::string_view delta) { text += delta; }));
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
