#include "runtime_test_support.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace scry::test_support;

namespace {

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

// The pump derives the callback text while committing, before the exchange
// moves into the Conversation, so these cases drive real deliveries rather
// than invoking the route directly.
[[nodiscard]] std::string delivered_text(PumpFixture& fixture, const std::uint64_t id,
                                         std::vector<scry::detail::Message> exchange) {
  scry::detail::PumpState pump{fixture.events};
  std::string observed;
  bool delivered = false;
  const auto route = fixture.route(
      id, {
              .callbacks =
                  scry::TurnCallbacks{
                      .on_finished =
                          [&observed, &delivered](scry::Result<scry::Completion> done) {
                            REQUIRE(done);
                            observed = done->text;
                            delivered = true;
                          },
                  },
          });
  pump.add_route(route);
  REQUIRE(fixture.events->push(
      scry::detail::CompletionEvent{
          .turn_id = route->id(),
          .exchange = std::move(exchange),
          .finish_reason = scry::FinishReason::completed,
      },
      1024));
  static_cast<void>(pump.update({}));
  REQUIRE(delivered);
  return observed;
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
  std::optional<scry::Error> delivered_error;
  const auto route = fixture.route(
      102, {
               .max_conversation_bytes = 1,
               .callbacks =
                   scry::TurnCallbacks{
                       .on_finished =
                           [&delivered_error](scry::Result<scry::Completion> done) {
                             if (!done) {
                               delivered_error = std::move(done.error());
                             }
                           },
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
      {
          .callbacks =
              scry::TurnCallbacks{
                  .on_text_delta =
                      [&delivered, &second_enqueue_succeeded, events = fixture.events,
                       turn_id](const std::string_view text) {
                        delivered.append(text);
                        if (text == "first") {
                          second_enqueue_succeeded = events->push(
                              scry::detail::TextDeltaEvent{.turn_id = turn_id,
                                                           .text = "second"},
                              1024);
                        }
                      },
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
      {
          .callbacks =
              scry::TurnCallbacks{
                  .on_text_delta =
                      [&pump, &nested, &queued_during_callback, events = fixture.events,
                       turn_id](const std::string_view text) {
                        if (text == "delta") {
                          queued_during_callback = events->push(
                              scry::detail::TextDeltaEvent{
                                  .turn_id = turn_id,
                                  .text = "later",
                              },
                              1024);
                          nested = pump.update({});
                        }
                      },
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
  const auto route =
      fixture.route(105, {
                             .callbacks =
                                 scry::TurnCallbacks{
                                     .on_text_delta = [](std::string_view) {},
                                 },
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

TEST_CASE("completion callback text concatenates only the exchange text blocks") {
  PumpFixture fixture;

  CHECK(delivered_text(fixture, 402, {}).empty());
  CHECK(delivered_text(fixture, 403,
                       {scry::detail::Message{
                           .role = scry::detail::Role::assistant,
                           .content = {tool_call("worker", "call-worker")},
                       }})
            .empty());
  CHECK(delivered_text(fixture, 404,
                       {scry::detail::Message{
                           .role = scry::detail::Role::assistant,
                           .content = {scry::detail::TextBlock{.text = "first "},
                                       tool_call("worker", "call-worker"),
                                       scry::detail::TextBlock{.text = "second"}},
                       }}) == "first second");
}

TEST_CASE("committing a completion moves its exchange into the Conversation") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  const auto route = fixture.route(
      405, {
               .callbacks =
                   scry::TurnCallbacks{
                       .on_finished = [](scry::Result<scry::Completion>) {},
                   },
           });
  pump.add_route(route);
  REQUIRE(fixture.events->push(completion_event(route->id()), 1024));

  static_cast<void>(pump.update({}));

  // The user message plus the committed assistant message, with content
  // intact after the move.
  REQUIRE(fixture.conversation->messages.size() == 2);
  const auto& assistant = fixture.conversation->messages.back();
  CHECK(assistant.role == scry::detail::Role::assistant);
  REQUIRE(assistant.content.size() == 1);
  CHECK(std::get<scry::detail::TextBlock>(assistant.content.front()).text == "done");
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
  bool completed = false;
  bool text_called = false;
  const auto route = fixture.route(
      215,
      {
          .callbacks = scry::TurnCallbacks{
              .on_text_delta = [&text_called](std::string_view) { text_called = true; },
              .on_finished =
                  [&completed](scry::Result<scry::Completion> done) {
                    completed = done.has_value();
                  },
          },
      });
  pump.add_route(route);
  REQUIRE(fixture.events->push(completion_event(route->id()), 1024));
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = route->id(), .text = "late"}, 1024));
  CHECK(pump.update({}).callbacks_delivered == 1);
  CHECK(completed);
  CHECK_FALSE(text_called);
  CHECK(route->terminal());
  CHECK(pump.live_route_count() == 0);
  CHECK(pump.find_route(route->id()));
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
  CHECK(pump.find_route(live->id()));

  const auto terminal = fixture.route(217);
  pump.add_route(terminal);
  terminal->detach();
  REQUIRE(fixture.events->push(completion_event(terminal->id()), 1024));
  CHECK(pump.update({}).events_remaining == 0);
  CHECK_FALSE(pump.find_route(terminal->id()));
}

TEST_CASE("detached callback routes remain until pending delivery completes") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  bool completed = false;
  const auto route = fixture.route(
      218, {
               .callbacks =
                   scry::TurnCallbacks{
                       .on_finished =
                           [&completed](scry::Result<scry::Completion> done) {
                             completed = done.has_value();
                           },
                   },
           });
  pump.add_route(route);
  route->detach();
  REQUIRE(fixture.events->push(completion_event(route->id()), 1024));

  const auto deferred = pump.update({.max_callbacks = 0});
  CHECK(deferred.events_remaining == 1);
  CHECK(deferred.budget_exhausted);
  CHECK(pump.find_route(route->id()));

  CHECK(pump.update({}).callbacks_delivered == 1);
  CHECK(completed);
  CHECK_FALSE(pump.find_route(route->id()));
}

TEST_CASE("events no callback can consume are released on arrival") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  constexpr std::size_t limit = 32;
  const auto route = fixture.route(219);
  pump.add_route(route);
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = route->id(), .text = "buffered"}, limit));
  REQUIRE(fixture.events->push_terminal(
      scry::detail::ErrorEvent{
          .turn_id = route->id(),
          .error = {.category = scry::ErrorCategory::protocol, .message = "failed"},
      },
      limit));

  const auto stats = pump.update({});
  CHECK(stats.callbacks_delivered == 0);
  CHECK(stats.events_remaining == 0);
  // The terminal still lands even with nothing observing it.
  CHECK(route->terminal());
  CHECK_FALSE(fixture.conversation->busy);
  // Both events returned their bytes, so the whole per-turn budget is free again.
  CHECK(fixture.events->push(
      scry::detail::TextDeltaEvent{
          .turn_id = route->id(),
          .text = std::string(limit, 'x'),
      },
      limit));
}

TEST_CASE("pump shutdown releases pending and queued event ownership") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  const auto route =
      fixture.route(220, {
                             .callbacks =
                                 scry::TurnCallbacks{
                                     .on_text_delta = [](std::string_view) {},
                                 },
                         });
  fixture.conversation->busy = true;
  pump.add_route(route);
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = route->id(), .text = "pending"}, 1024));
  CHECK(pump.update({.max_callbacks = 0}).events_remaining == 1);
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = route->id(), .text = "queued"}, 1024));

  pump.shutdown();

  CHECK_FALSE(fixture.conversation->busy);
  CHECK_FALSE(pump.find_route(route->id()));
  CHECK(fixture.events->size() == 0);
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{
          .turn_id = route->id(),
          .text = std::string(1024, 'x'),
      },
      1024));
}
