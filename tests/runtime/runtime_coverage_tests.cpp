#include "runtime/config.hpp"
#include "runtime/messages.hpp"
#include "runtime/pump.hpp"
#include "runtime/state.hpp"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <scry/conversation.hpp>
#include <scry/error.hpp>
#include <scry/events.hpp>
#include <scry/json.hpp>
#include <scry/tool_registry.hpp>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace std::chrono_literals;

namespace {

[[nodiscard]] scry::Config valid_config() {
  return {
      .base_url = "https://example.test",
      .api_key = "test-key",
      .model = "test-model",
  };
}

struct PumpFixture {
  std::shared_ptr<scry::detail::CommandQueue> commands{
      std::make_shared<scry::detail::CommandQueue>()};
  std::shared_ptr<scry::detail::EventQueue> events{
      std::make_shared<scry::detail::EventQueue>()};
  std::shared_ptr<scry::detail::ConversationState> conversation{
      std::make_shared<scry::detail::ConversationState>()};

  [[nodiscard]] std::shared_ptr<scry::detail::TurnRoute>
  route(const std::uint64_t value, const std::size_t conversation_limit = 1024) const {
    return std::make_shared<scry::detail::TurnRoute>(
        scry::TurnId{.value = value}, std::make_shared<std::atomic<bool>>(false),
        commands, conversation, "question",
        scry::detail::TurnRouteOptions{
            .max_tool_result_bytes = 1024,
            .max_conversation_bytes = conversation_limit,
        });
  }
};

[[nodiscard]] scry::detail::CompletionEvent completion(const scry::TurnId turn_id,
                                                       std::string text = "answer") {
  return {
      .turn_id = turn_id,
      .exchange = {scry::detail::Message{
          .role = scry::detail::Role::assistant,
          .content = {scry::detail::TextBlock{.text = std::move(text)}},
      }},
      .finish_reason = scry::FinishReason::completed,
      .attempt_count = 2,
      .provider_request_id = "request-id",
  };
}

void check_registration_error(const scry::Status& status) {
  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::invalid_state);
}

} // namespace

TEST_CASE("configuration checks every resource-limit operand") {
  constexpr std::array limits{
      &scry::ResourceLimits::max_pending_turns,
      &scry::ResourceLimits::max_sse_event_bytes,
      &scry::ResourceLimits::max_response_bytes,
      &scry::ResourceLimits::max_tool_arguments_bytes,
      &scry::ResourceLimits::max_tool_result_bytes,
      &scry::ResourceLimits::max_queued_event_bytes_per_turn,
      &scry::ResourceLimits::max_conversation_bytes,
  };
  for (const auto member : limits) {
    auto config = valid_config();
    config.limits.*member = 0;
    const auto status = scry::detail::validate_config(config);
    REQUIRE_FALSE(status);
    CHECK(status.error().category == scry::ErrorCategory::invalid_config);
  }
}

TEST_CASE("configuration checks compound endpoint and sampling alternatives") {
  auto config = valid_config();
  config.base_url = "https://example .test";
  CHECK_FALSE(scry::detail::validate_config(config));
  config = valid_config();
  config.sampling.temperature = -0.1;
  CHECK_FALSE(scry::detail::validate_config(config));
  config = valid_config();
  config.sampling.top_p = std::numeric_limits<double>::quiet_NaN();
  CHECK_FALSE(scry::detail::validate_config(config));
  config = valid_config();
  config.sampling.top_p = 1.1;
  CHECK_FALSE(scry::detail::validate_config(config));
}

TEST_CASE("configuration checks each retry policy boundary") {
  auto config = valid_config();
  config.retry.initial_backoff = -1ms;
  CHECK_FALSE(scry::detail::validate_config(config));
  config = valid_config();
  config.retry.max_backoff = -1ms;
  CHECK_FALSE(scry::detail::validate_config(config));
  config = valid_config();
  config.retry.max_elapsed = -1ms;
  CHECK_FALSE(scry::detail::validate_config(config));
  config = valid_config();
  config.retry.jitter_ratio = std::numeric_limits<double>::infinity();
  CHECK_FALSE(scry::detail::validate_config(config));
  config = valid_config();
  config.retry.jitter_ratio = -0.1;
  CHECK_FALSE(scry::detail::validate_config(config));
  config = valid_config();
  config.retry.jitter_ratio = 1.1;
  CHECK_FALSE(scry::detail::validate_config(config));
}

TEST_CASE("configuration checks each runtime-bound alternative") {
  auto config = valid_config();
  config.timeouts.connect = 0ms;
  CHECK_FALSE(scry::detail::validate_config(config));
  config = valid_config();
  config.timeouts.transfer = 0ms;
  CHECK_FALSE(scry::detail::validate_config(config));
  config = valid_config();
  config.max_tool_rounds = 0;
  CHECK_FALSE(scry::detail::validate_config(config));
}

TEST_CASE("message and event payload accounting visits every content block") {
  const scry::detail::Message message{
      .role = scry::detail::Role::assistant,
      .content =
          {
              scry::detail::TextBlock{.text = "abc"},
              scry::detail::ToolCallBlock{
                  .id = "id",
                  .name = "tool",
                  .arguments = {.text = "input"},
              },
              scry::detail::ToolResultBlock{
                  .tool_call_id = "ref",
                  .result = {.text = "json"},
                  .is_error = true,
              },
          },
  };
  constexpr std::size_t message_bytes = 3 + 2 + 4 + 5 + 3 + 4 + sizeof(bool);
  CHECK(scry::detail::message_payload_bytes(message) == message_bytes);
  const scry::detail::ModelResponse response{
      .content = message.content,
      .provider_request_id = "req",
  };
  const scry::detail::WorkerEvent event{scry::detail::CompletionEvent{
      .turn_id = {.value = 201},
      .exchange = {message},
      .attempt_count = 1,
      .provider_request_id = "req",
  }};
  CHECK(scry::detail::event_payload_bytes(event) ==
        message_bytes + std::string_view{"req"}.size());
  CHECK(scry::detail::response_text(response) == "abc");
}

TEST_CASE("event metadata accounts for every event alternative") {
  const auto turn_id = scry::TurnId{.value = 202};
  const scry::detail::WorkerEvent text{
      scry::detail::TextDeltaEvent{.turn_id = turn_id, .text = "text"}};
  const scry::detail::WorkerEvent error{scry::detail::ErrorEvent{
      .turn_id = turn_id,
      .error =
          {
              .message = "message",
              .provider_detail = "detail",
              .provider_request_id = "request",
          },
  }};
  const scry::detail::WorkerEvent cancelled{
      scry::detail::CancelledEvent{.turn_id = turn_id}};
  CHECK(scry::detail::event_turn_id(text) == turn_id);
  CHECK(scry::detail::event_payload_bytes(text) == 4);
  CHECK(scry::detail::event_payload_bytes(error) == 20);
  CHECK(scry::detail::event_payload_bytes(cancelled) == 0);
}

TEST_CASE("turn route rejects empty and duplicate callback registrations") {
  PumpFixture fixture;
  const auto route = fixture.route(212);
  check_registration_error(route->register_text({}));
  REQUIRE(route->register_text([](std::string_view) {}));
  check_registration_error(route->register_text([](std::string_view) {}));
  check_registration_error(route->register_tool({}));
  REQUIRE(route->register_tool([](const scry::ToolCall&) {}));
  check_registration_error(route->register_tool([](const scry::ToolCall&) {}));
  check_registration_error(route->register_completion({}));
  REQUIRE(route->register_completion([](const scry::Completion&) {}));
  check_registration_error(route->register_completion([](const scry::Completion&) {}));
  check_registration_error(route->register_error({}));
  REQUIRE(route->register_error([](const scry::Error&) {}));
  check_registration_error(route->register_error([](const scry::Error&) {}));
  check_registration_error(route->register_cancelled({}));
  REQUIRE(route->register_cancelled([](const scry::Cancelled&) {}));
  check_registration_error(route->register_cancelled([](const scry::Cancelled&) {}));
}

TEST_CASE("turn route dispatches every terminal and text worker event callback") {
  PumpFixture fixture;
  const auto route = fixture.route(213);
  std::string text;
  std::optional<scry::Completion> completed;
  std::optional<scry::Error> failed;
  std::optional<scry::Cancelled> cancelled;
  const scry::detail::WorkerEvent text_event{
      scry::detail::TextDeltaEvent{.turn_id = route->id(), .text = "delta"}};
  const scry::detail::WorkerEvent completion_event{completion(route->id())};
  const scry::detail::WorkerEvent error_event{scry::detail::ErrorEvent{
      .turn_id = route->id(),
      .error = {.category = scry::ErrorCategory::network, .message = "failure"},
  }};
  const scry::detail::WorkerEvent cancelled_event{
      scry::detail::CancelledEvent{.turn_id = route->id()}};
  CHECK_FALSE(route->has_callback(text_event));
  CHECK_FALSE(route->has_callback(completion_event));
  CHECK_FALSE(route->has_callback(error_event));
  CHECK_FALSE(route->has_callback(cancelled_event));
  REQUIRE(
      route->register_text([&text](const std::string_view value) { text = value; }));
  REQUIRE(route->register_completion(
      [&completed](const scry::Completion& value) { completed = value; }));
  REQUIRE(
      route->register_error([&failed](const scry::Error& value) { failed = value; }));
  REQUIRE(route->register_cancelled(
      [&cancelled](const scry::Cancelled& value) { cancelled = value; }));
  route->invoke(text_event);
  route->invoke(completion_event);
  route->invoke(error_event);
  route->invoke(cancelled_event);
  CHECK(text == "delta");
  REQUIRE(completed);
  CHECK(completed->attempt_count == 2);
  REQUIRE(failed);
  CHECK(failed->message == "failure");
  REQUIRE(cancelled);
  CHECK(cancelled->turn_id == route->id());
}

TEST_CASE("pump releases events without routes and events after terminal state") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  const auto missing = scry::TurnId{.value = 214};
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = missing, .text = "orphan"}, 16));
  CHECK(pump.update({}).events_remaining == 0);
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = missing, .text = std::string(16, 'x')},
      16));
  const auto route = fixture.route(215);
  pump.add_route(route);
  bool completed = false;
  bool text_called = false;
  REQUIRE(route->register_completion(
      [&completed](const scry::Completion&) { completed = true; }));
  REQUIRE(
      route->register_text([&text_called](std::string_view) { text_called = true; }));
  REQUIRE(fixture.events->push(completion(route->id()), 1024));
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = route->id(), .text = "late"}, 1024));
  CHECK(pump.update({}).callbacks_delivered == 1);
  CHECK(completed);
  CHECK_FALSE(text_called);
  CHECK(route->terminal());
  CHECK(pump.live_route_count() == 0);
  CHECK(pump.route_count() == 1);
  CHECK_FALSE(pump.find_route(scry::TurnId{.value = 999}));
}

TEST_CASE("pump discards detached unclaimed events and cleans terminal routes") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  const auto live = fixture.route(216);
  pump.add_route(live);
  live->detach();
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = live->id(), .text = "unclaimed"}, 1024));
  CHECK(pump.update({}).events_remaining == 0);
  CHECK(pump.route_count() == 1);

  const auto terminal = fixture.route(217);
  pump.add_route(terminal);
  terminal->detach();
  REQUIRE(fixture.events->push(completion(terminal->id()), 1024));
  CHECK(pump.update({}).events_remaining == 0);
  CHECK_FALSE(pump.find_route(terminal->id()));
}

TEST_CASE("detached callback routes remain until pending delivery completes") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  const auto route = fixture.route(218);
  pump.add_route(route);
  route->detach();
  bool completed = false;
  REQUIRE(route->register_completion(
      [&completed](const scry::Completion&) { completed = true; }));
  REQUIRE(fixture.events->push(completion(route->id()), 1024));

  const auto deferred = pump.update({.max_callbacks = 0});
  CHECK(deferred.events_remaining == 1);
  CHECK(deferred.budget_exhausted);
  CHECK(pump.route_count() == 1);

  CHECK(pump.update({}).callbacks_delivered == 1);
  CHECK(completed);
  CHECK(pump.route_count() == 0);
}

TEST_CASE("late callback registration receives buffered text and error events") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  const auto route = fixture.route(219);
  pump.add_route(route);
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = route->id(), .text = "buffered"}, 1024));
  fixture.events->push_terminal(scry::detail::ErrorEvent{
      .turn_id = route->id(),
      .error = {.category = scry::ErrorCategory::protocol, .message = "failed"},
  });
  CHECK(pump.update({}).callbacks_delivered == 0);

  std::string text;
  std::string error;
  REQUIRE(
      route->register_text([&text](const std::string_view value) { text = value; }));
  REQUIRE(route->register_error(
      [&error](const scry::Error& value) { error = value.message; }));
  CHECK(pump.update({}).callbacks_delivered == 2);
  CHECK(text == "buffered");
  CHECK(error == "failed");
}

TEST_CASE("pump shutdown releases pending and queued event ownership") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  const auto route = fixture.route(220);
  fixture.conversation->busy = true;
  pump.add_route(route);
  REQUIRE(route->register_text([](std::string_view) {}));
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = route->id(), .text = "pending"}, 1024));
  CHECK(pump.update({.max_callbacks = 0}).events_remaining == 1);
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = route->id(), .text = "queued"}, 1024));

  pump.shutdown();

  CHECK_FALSE(fixture.conversation->busy);
  CHECK(pump.route_count() == 0);
  CHECK(fixture.events->size() == 0);
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{
          .turn_id = route->id(),
          .text = std::string(1024, 'x'),
      },
      1024));
}

TEST_CASE("turn route cancellation remains safe after its command queue expires") {
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

TEST_CASE("moved-from Conversation observers remain harmless") {
  auto source = scry::Conversation::create();
  REQUIRE(source);
  auto active = std::move(*source);

  CHECK(source->empty());
  CHECK(source->message_count() == 0);
  CHECK(active.empty());
  CHECK(active.message_count() == 0);
}
