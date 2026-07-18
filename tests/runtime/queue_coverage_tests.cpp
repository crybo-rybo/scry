#include "core/model.hpp"
#include "runtime/queue.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <limits>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace std::chrono_literals;

TEST_CASE("payload byte accounting saturates instead of wrapping") {
  constexpr auto maximum = std::numeric_limits<std::size_t>::max();
  CHECK(scry::detail::saturating_payload_add(maximum - 1, 2) == maximum);
}

TEST_CASE("event queue coalescing handles non-adjacent and cross-turn events") {
  scry::detail::EventQueue queue;
  const auto first = scry::TurnId{.value = 203};
  const auto second = scry::TurnId{.value = 204};
  REQUIRE(queue.push_terminal(
      scry::detail::ErrorEvent{.turn_id = first, .error = {.message = "e"}}, 16));
  REQUIRE(queue.push(scry::detail::TextDeltaEvent{.turn_id = first, .text = "a"}, 16));
  REQUIRE(queue.push(scry::detail::TextDeltaEvent{.turn_id = second, .text = "b"}, 16));
  CHECK(queue.size() == 3);
}

TEST_CASE("event queue admits or rejects a same-turn batch atomically") {
  scry::detail::EventQueue queue;
  const auto turn_id = scry::TurnId{.value = 230};
  CHECK(queue.push_batch({}, 0));
  std::vector<scry::detail::WorkerEvent> oversized{
      scry::detail::TextDeltaEvent{.turn_id = turn_id, .text = "12"},
      scry::detail::TextDeltaEvent{.turn_id = turn_id, .text = "345"},
  };
  CHECK_FALSE(queue.push_batch(std::move(oversized), 4));
  CHECK(queue.size() == 0);

  std::vector<scry::detail::WorkerEvent> accepted{
      scry::detail::TextDeltaEvent{.turn_id = turn_id, .text = "12"},
      scry::detail::TextDeltaEvent{.turn_id = turn_id, .text = "34"},
  };
  REQUIRE(queue.push_batch(std::move(accepted), 4));
  CHECK(queue.size() == 2);
  std::vector<scry::detail::WorkerEvent> no_remaining_capacity{
      scry::detail::TextDeltaEvent{.turn_id = turn_id, .text = "x"},
  };
  CHECK_FALSE(queue.push_batch(std::move(no_remaining_capacity), 4));
  CHECK(queue.size() == 2);
}

TEST_CASE("event queue batch honors a reduced limit after ownership transfers") {
  scry::detail::EventQueue queue;
  const auto turn_id = scry::TurnId{.value = 233};
  REQUIRE(
      queue.push(scry::detail::TextDeltaEvent{.turn_id = turn_id, .text = "1234"}, 4));
  REQUIRE(queue.try_pop());
  std::vector<scry::detail::WorkerEvent> batch{
      scry::detail::TextDeltaEvent{.turn_id = turn_id, .text = ""},
  };
  CHECK_FALSE(queue.push_batch(std::move(batch), 3));
}

TEST_CASE("event queue rejects batches that mix turn ownership") {
  scry::detail::EventQueue queue;
  std::vector<scry::detail::WorkerEvent> mixed{
      scry::detail::TextDeltaEvent{.turn_id = {.value = 231}, .text = "a"},
      scry::detail::TextDeltaEvent{.turn_id = {.value = 232}, .text = "b"},
  };
  CHECK_FALSE(queue.push_batch(std::move(mixed), 4));
  CHECK(queue.size() == 0);
}

TEST_CASE("event queue rejects coalescing against a reduced byte limit") {
  scry::detail::EventQueue queue;
  const auto turn_id = scry::TurnId{.value = 205};
  REQUIRE(
      queue.push(scry::detail::TextDeltaEvent{.turn_id = turn_id, .text = "1234"}, 4));
  CHECK_FALSE(
      queue.push(scry::detail::TextDeltaEvent{.turn_id = turn_id, .text = "x"}, 3));
  CHECK_FALSE(queue.push_terminal(scry::detail::CancelledEvent{.turn_id = turn_id}, 3));
}

TEST_CASE("event queue rejects a coalesced delta beyond remaining capacity") {
  scry::detail::EventQueue queue;
  const auto turn_id = scry::TurnId{.value = 206};
  REQUIRE(
      queue.push(scry::detail::TextDeltaEvent{.turn_id = turn_id, .text = "123"}, 4));
  CHECK_FALSE(
      queue.push(scry::detail::TextDeltaEvent{.turn_id = turn_id, .text = "xy"}, 4));
}

TEST_CASE("event queue discard preserves bytes already owned by the pump") {
  scry::detail::EventQueue queue;
  const auto target = scry::TurnId{.value = 207};
  const auto other = scry::TurnId{.value = 208};
  REQUIRE(
      queue.push(scry::detail::TextDeltaEvent{.turn_id = target, .text = "held"}, 32));
  auto held = queue.try_pop();
  REQUIRE(held);
  REQUIRE(queue.push_terminal(
      scry::detail::ErrorEvent{.turn_id = target, .error = {.message = "queued"}}, 32));
  REQUIRE(
      queue.push(scry::detail::TextDeltaEvent{.turn_id = other, .text = "other"}, 32));
  queue.discard(scry::TurnId{.value = 999});
  CHECK(queue.size() == 2);
  queue.discard(target);
  CHECK(queue.size() == 1);
  REQUIRE(queue.push(
      scry::detail::TextDeltaEvent{.turn_id = target, .text = std::string(28, 'x')},
      32));
  queue.release(*held);
}

TEST_CASE("event queue release retains and then clears remaining accounting") {
  scry::detail::EventQueue queue;
  const auto turn_id = scry::TurnId{.value = 209};
  REQUIRE(queue.push(scry::detail::TextDeltaEvent{.turn_id = turn_id, .text = "first"},
                     16));
  auto first = queue.try_pop();
  REQUIRE(first);
  REQUIRE(queue.push_terminal(
      scry::detail::ErrorEvent{.turn_id = turn_id, .error = {.message = "last"}}, 16));
  auto last = queue.try_pop();
  REQUIRE(last);
  queue.release(*first);
  CHECK_FALSE(queue.push(
      scry::detail::TextDeltaEvent{.turn_id = turn_id, .text = std::string(13, 'x')},
      16));
  queue.release(*last);
  REQUIRE(queue.push(
      scry::detail::TextDeltaEvent{.turn_id = turn_id, .text = std::string(16, 'x')},
      16));
  queue.release(scry::TurnId{.value = 999}, 1);
}

TEST_CASE("blocking queue exposes timeout and size behavior") {
  scry::detail::CommandQueue queue;
  std::stop_source stopped;
  CHECK(queue.size() == 0);
  CHECK_FALSE(
      queue.wait_pop_until(stopped.get_token(), std::chrono::steady_clock::now()));
  queue.push(scry::detail::CancelTurnCommand{.turn_id = {.value = 210}});
  CHECK(queue.size() == 1);
  const auto command =
      queue.wait_pop_until(stopped.get_token(), std::chrono::steady_clock::now() + 1s);
  REQUIRE(command);
  CHECK(std::get<scry::detail::CancelTurnCommand>(*command).turn_id.value == 210);
  CHECK(queue.size() == 0);
}

TEST_CASE("event queue wait reports timeout and ready data") {
  scry::detail::EventQueue queue;
  CHECK_FALSE(queue.wait_for_data(0ms));
  queue.push_terminal(scry::detail::CancelledEvent{.turn_id = {.value = 211}});
  CHECK(queue.wait_for_data(0ms));
}
