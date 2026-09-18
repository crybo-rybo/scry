#include "runtime_test_support.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace scry::test_support;

namespace {

// An error whose diagnostic alone fills most of a tightened per-turn budget.
[[nodiscard]] scry::detail::ErrorEvent oversized_error(const scry::TurnId turn_id) {
  return {
      .turn_id = turn_id,
      .error =
          {
              .category = scry::ErrorCategory::resource_limit,
              .message = std::string(128, 'a'),
              .turn_id = turn_id,
          },
  };
}

// A completion carrying far more transcript than the queue budget would allow
// if the transcript were charged to it.
[[nodiscard]] scry::detail::CompletionEvent
large_completion(const scry::TurnId turn_id) {
  return {
      .turn_id = turn_id,
      .transcript = {scry::detail::Message{
          .role = scry::detail::Role::assistant,
          .content = {scry::detail::TextBlock{
              .text = std::string(4096, 'a'),
          }},
      }},
      .finish_reason = scry::detail::FinishReason::completed,
      .attempt_count = 1,
      .provider_request_id = "request-id",
  };
}

// The pump derives the callback text while committing, before the transcript
// moves into the Conversation, so these cases drive real deliveries rather
// than invoking the route directly.
[[nodiscard]] std::string
delivered_text(PumpFixture& fixture, const std::uint64_t id,
               std::vector<scry::detail::Message> transcript) {
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
          .transcript = std::move(transcript),
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

// Delivery credits the bytes measured when the event arrived, so the per-turn
// ledger drains exactly. Errors are the charged terminal event now that a
// completion costs only its correlation id.
TEST_CASE("delivering a terminal error releases the originally accounted queue bytes") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  std::optional<scry::Error> delivered_error;
  const auto route = fixture.route(
      102, {
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

  REQUIRE(fixture.events->push_terminal(oversized_error(route->id()), 256));

  const auto stats = pump.update({});
  CHECK(stats.callbacks_delivered == 1);
  REQUIRE(delivered_error);
  CHECK(delivered_error->category == scry::ErrorCategory::resource_limit);
  CHECK(fixture.conversation->messages->empty());

  // The 128 diagnostic bytes returned to the ledger, so a 256-byte delta fits
  // again under the same per-turn limit.
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{
          .turn_id = route->id(),
          .text = std::string(256, 'b'),
      },
      256));
}

TEST_CASE("a queued completion consumes only its correlation id bytes") {
  scry::detail::EventQueue queue;
  const auto turn_id = scry::TurnId{.value = 106};
  constexpr std::size_t limit = 256 + std::string_view{"request-id"}.size();

  REQUIRE(queue.push(large_completion(turn_id), limit));

  CHECK(queue.push(
      scry::detail::TextDeltaEvent{.turn_id = turn_id, .text = std::string(256, 'c')},
      limit));
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

TEST_CASE("a nonpositive pump budget still ingests and delivers one unit of work") {
  PumpFixture fixture;
  auto now = std::chrono::steady_clock::time_point{};
  scry::detail::PumpState pump{fixture.events, [&now] { return now; }};
  // Deltas for the same turn coalesce, so two turns are needed to observe two
  // separate ingests.
  const auto first =
      fixture.route(105, {
                             .callbacks =
                                 scry::TurnCallbacks{
                                     .on_text_delta = [](std::string_view) {},
                                 },
                         });
  const auto second =
      fixture.route(106, {
                             .callbacks =
                                 scry::TurnCallbacks{
                                     .on_text_delta = [](std::string_view) {},
                                 },
                         });
  pump.add_route(first);
  pump.add_route(second);
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = first->id(), .text = "first"}, 1024));
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = second->id(), .text = "second"}, 1024));

  // An expired budget buys one ingest and one delivery, never zero progress.
  const auto bounded = pump.update({.time_budget = std::chrono::microseconds{0}});
  CHECK(bounded.callbacks_delivered == 1);
  CHECK(bounded.events_remaining == 1);
  CHECK(bounded.budget_exhausted);

  const auto drained = pump.update({.time_budget = std::chrono::microseconds{0}});
  CHECK(drained.callbacks_delivered == 1);
  CHECK(drained.events_remaining == 0);
}

TEST_CASE("completion callback text concatenates only the final message text blocks") {
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

TEST_CASE("a delivered terminal carries its whole payload to on_finished") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  std::optional<scry::Completion> completed;
  const auto completing = fixture.route(
      406, {
               .callbacks =
                   scry::TurnCallbacks{
                       .on_finished =
                           [&completed](scry::Result<scry::Completion> outcome) {
                             REQUIRE(outcome);
                             completed = std::move(*outcome);
                           },
                   },
           });
  pump.add_route(completing);
  REQUIRE(fixture.events->push_terminal(
      completion_event(completing->id(), {.text = "final answer",
                                          .attempt_count = 3,
                                          .provider_request_id = "req-4242"}),
      1024));

  CHECK(pump.update({}).callbacks_delivered == 1);

  REQUIRE(completed);
  CHECK(completed->turn_id == completing->id());
  CHECK(completed->text == "final answer");
  CHECK(completed->provider_request_id == "req-4242");
  CHECK(completed->attempt_count == 3);
  CHECK(completed->finish_reason == scry::FinishReason::completed);

  std::optional<scry::Error> failed;
  const auto failing = fixture.route(
      407, {
               .callbacks =
                   scry::TurnCallbacks{
                       .on_finished =
                           [&failed](scry::Result<scry::Completion> outcome) {
                             REQUIRE_FALSE(outcome);
                             failed = std::move(outcome.error());
                           },
                   },
           });
  pump.add_route(failing);
  REQUIRE(fixture.events->push_terminal(
      scry::detail::ErrorEvent{
          .turn_id = failing->id(),
          .error =
              {
                  .category = scry::ErrorCategory::rate_limit,
                  .attempt = 2,
                  .message = "upstream refused",
                  .provider_detail = "overloaded_error",
                  .turn_id = failing->id(),
                  .provider_request_id = "req-9",
                  .model_message = "try again later",
              },
      },
      1024));

  CHECK(pump.update({}).callbacks_delivered == 1);

  REQUIRE(failed);
  CHECK(failed->category == scry::ErrorCategory::rate_limit);
  CHECK(failed->attempt == 2);
  CHECK(failed->message == "upstream refused");
  CHECK(failed->provider_detail == "overloaded_error");
  CHECK(failed->turn_id == failing->id());
  CHECK(failed->provider_request_id == "req-9");
  CHECK(failed->model_message == "try again later");
}

TEST_CASE("a callback limit reports an exhausted budget only when work remains") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  std::shared_ptr<scry::detail::TurnRoute> second;
  std::string first_text;
  std::string second_text;
  const auto first = fixture.route(
      408, {
               .callbacks =
                   scry::TurnCallbacks{
                       .on_text_delta =
                           [&](const std::string_view delta) { first_text += delta; },
                   },
           });
  second = fixture.route(
      409, {
               .callbacks =
                   scry::TurnCallbacks{
                       .on_text_delta =
                           [&](const std::string_view delta) { second_text += delta; },
                   },
           });
  pump.add_route(first);
  pump.add_route(second);
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = first->id(), .text = "first"}, 1024));
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = second->id(), .text = "second"}, 1024));

  // One delivery of two: the second entry is still deliverable, so the limit
  // was what stopped the call.
  const auto limited = pump.update({.max_callbacks = 1});
  CHECK(limited.callbacks_delivered == 1);
  CHECK(limited.events_remaining == 1);
  CHECK(limited.budget_exhausted);
  CHECK(first_text == "first");

  // One delivery of one: nothing is left over, so the limit is not reported as
  // exhausted even though it was reached exactly.
  const auto drained = pump.update({.max_callbacks = 1});
  CHECK(drained.callbacks_delivered == 1);
  CHECK(drained.events_remaining == 0);
  CHECK_FALSE(drained.budget_exhausted);
  CHECK(second_text == "second");
}

TEST_CASE("a callback limit ignores an entry no route can consume any more") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  std::shared_ptr<scry::detail::TurnRoute> second;
  bool disconnected = false;
  const auto first = fixture.route(
      410,
      {
          .callbacks =
              scry::TurnCallbacks{
                  .on_text_delta =
                      [&](std::string_view) { disconnected = second->disconnect(); },
              },
      });
  second = fixture.route(
      411, {
               .callbacks =
                   scry::TurnCallbacks{
                       .on_text_delta = [](std::string_view) { FAIL("delivered"); },
                   },
           });
  pump.add_route(first);
  pump.add_route(second);
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = first->id(), .text = "first"}, 1024));
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = second->id(), .text = "second"}, 1024));

  // The only delivery disconnects the other route, so the entry left pending is
  // dead rather than owed: reaching the limit exactly is not an exhausted budget.
  const auto stats = pump.update({.max_callbacks = 1});
  CHECK(disconnected);
  CHECK(stats.callbacks_delivered == 1);
  CHECK(stats.events_remaining == 0);
  CHECK_FALSE(stats.budget_exhausted);
}

TEST_CASE("committing a completion moves its transcript into the Conversation") {
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
  REQUIRE(fixture.conversation->messages->size() == 2);
  const auto& assistant = fixture.conversation->messages->back();
  CHECK(assistant.role == scry::detail::Role::assistant);
  REQUIRE(assistant.content.size() == 1);
  CHECK(std::get<scry::detail::TextBlock>(assistant.content.front()).text == "done");
}

TEST_CASE("commit reseats history while a retained snapshot stays immutable") {
  PumpFixture fixture;
  fixture.conversation->messages->push_back(scry::detail::Message{
      .role = scry::detail::Role::assistant,
      .content = {scry::detail::TextBlock{.text = "committed"}},
  });
  const auto snapshot = fixture.conversation->messages;
  const auto* original_block = snapshot.get();
  scry::detail::PumpState pump{fixture.events};
  const auto route = fixture.route(406);
  pump.add_route(route);
  REQUIRE(fixture.events->push(completion_event(route->id()), 1024));

  static_cast<void>(pump.update({}));

  REQUIRE(snapshot->size() == 1);
  CHECK(snapshot.get() == original_block);
  CHECK(fixture.conversation->messages.get() != original_block);
  REQUIRE(fixture.conversation->messages->size() == 3);
  CHECK(std::get<scry::detail::TextBlock>(snapshot->front().content.front()).text ==
        "committed");
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

TEST_CASE("a disconnected route keeps its turn but delivers nothing") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  constexpr std::size_t limit = 64;
  std::string streamed;
  bool finished = false;
  const auto route = fixture.route(
      221, {
               .callbacks = scry::TurnCallbacks{
                   .on_text_delta =
                       [&streamed](std::string_view delta) { streamed.append(delta); },
                   .on_finished =
                       [&finished](scry::Result<scry::Completion>) { finished = true; },
               },
           });
  pump.add_route(route);
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = route->id(), .text = "streamed"}, limit));
  REQUIRE(fixture.events->push_terminal(completion_event(route->id()), limit));
  // Ingest both without delivering either, so the disconnect below lands on a
  // turn whose terminal outcome is queued but not yet reported.
  CHECK(pump.update({.max_callbacks = 0}).events_remaining == 2);

  CHECK(route->disconnect());
  CHECK_FALSE(route->disconnect());

  const auto stats = pump.update({});
  CHECK(stats.callbacks_delivered == 0);
  // Both queued events were released rather than delivered, so the whole
  // per-turn budget is free again.
  CHECK(stats.events_remaining == 0);
  CHECK(streamed.empty());
  CHECK_FALSE(finished);
  // The turn itself ran to completion: history committed and busy cleared.
  REQUIRE(fixture.conversation->messages->size() == 2);
  CHECK(std::get<scry::detail::TextBlock>(
            fixture.conversation->messages->back().content.front())
            .text == "done");
  CHECK_FALSE(fixture.conversation->busy);
  // With no delivery left to wait for, the turn is finished as soon as the
  // terminal event is processed.
  CHECK(route->finished());
  CHECK(fixture.events->push(
      scry::detail::TextDeltaEvent{
          .turn_id = route->id(),
          .text = std::string(limit, 'x'),
      },
      limit));
}

TEST_CASE("a route that already reported its outcome cannot be disconnected") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  bool finished = false;
  const auto route = fixture.route(
      222,
      {
          .callbacks =
              scry::TurnCallbacks{
                  .on_finished =
                      [&finished](scry::Result<scry::Completion>) { finished = true; },
              },
      });
  pump.add_route(route);
  REQUIRE(fixture.events->push_terminal(completion_event(route->id()), 1024));

  CHECK(pump.update({}).callbacks_delivered == 1);

  REQUIRE(finished);
  CHECK_FALSE(route->disconnect());
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

TEST_CASE("disconnecting from inside on_text_delta does not destroy the running "
          "callback") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  std::shared_ptr<scry::detail::TurnRoute> route;
  std::size_t observed_size = 0;
  std::size_t delta_calls = 0;
  bool disconnect_reported = false;
  bool finished = false;
  route = fixture.route(
      223, {
               .callbacks = scry::TurnCallbacks{
                   .on_text_delta =
                       [&route, &observed_size, &delta_calls, &disconnect_reported,
                        captured = std::string(64, 'x')](std::string_view) {
                         ++delta_calls;
                         disconnect_reported = route->disconnect();
                         // Reading a capture after the disconnect is the whole
                         // regression: clearing the callbacks eagerly deleted the
                         // closure this frame is running out of.
                         observed_size = captured.size();
                       },
                   .on_finished =
                       [&finished](scry::Result<scry::Completion>) { finished = true; },
               },
           });
  pump.add_route(route);
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = route->id(), .text = "streamed"}, 1024));
  REQUIRE(fixture.events->push_terminal(completion_event(route->id()), 1024));

  static_cast<void>(pump.update({}));

  CHECK(delta_calls == 1);
  CHECK(disconnect_reported);
  CHECK(observed_size == 64);
  // The deferred clear still stops delivery at the very next event.
  CHECK_FALSE(finished);
  // The turn itself ran to completion: history committed and busy cleared.
  REQUIRE(fixture.conversation->messages->size() == 2);
  CHECK(std::get<scry::detail::TextBlock>(
            fixture.conversation->messages->back().content.front())
            .text == "done");
  CHECK_FALSE(fixture.conversation->busy);
}

TEST_CASE("disconnecting from inside on_tool_call does not destroy the running "
          "callback") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  const scry::detail::ToolSnapshot tools{
      registered_tool("forecast", [](scry::Json) -> scry::Result<scry::Json> {
        return scry::Json{.text = R"({"ok":true})"};
      })};
  std::shared_ptr<scry::detail::TurnRoute> route;
  std::size_t observed_size = 0;
  std::size_t observer_calls = 0;
  bool disconnect_reported = false;
  route = fixture.route(
      224,
      {
          .tools = frozen_tools(tools),
          .callbacks =
              scry::TurnCallbacks{
                  .on_tool_call =
                      [&route, &observed_size, &observer_calls, &disconnect_reported,
                       captured = std::string(64, 'y')](const scry::ToolCall&) {
                        ++observer_calls;
                        disconnect_reported = route->disconnect();
                        observed_size = captured.size();
                      },
              },
      });
  pump.add_route(route);
  REQUIRE(fixture.events->push(tool_event(route->id()), 1024));

  CHECK(pump.update({}).callbacks_delivered == 1);

  CHECK(observer_calls == 1);
  CHECK(disconnect_reported);
  CHECK(observed_size == 64);
  // Tool dispatch belongs to the registry, so the result still reached the queue.
  REQUIRE(fixture.commands->try_pop());
}

TEST_CASE("disconnecting from inside a tool handler suppresses that call's observer") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  std::shared_ptr<scry::detail::TurnRoute> route;
  std::size_t observer_calls = 0;
  bool disconnect_reported = false;
  const scry::detail::ToolSnapshot tools{registered_tool(
      "forecast",
      [&route, &disconnect_reported](scry::Json) -> scry::Result<scry::Json> {
        disconnect_reported = route->disconnect();
        return scry::Json{.text = R"({"ok":true})"};
      })};
  route = fixture.route(
      226, {
               .tools = frozen_tools(tools),
               .callbacks =
                   scry::TurnCallbacks{
                       .on_tool_call = [&observer_calls](
                                           const scry::ToolCall&) { ++observer_calls; },
                   },
           });
  pump.add_route(route);
  REQUIRE(fixture.events->push(tool_event(route->id()), 1024));

  static_cast<void>(pump.update({}));

  CHECK(disconnect_reported);
  // The handler ran inside this dispatch, so the deferred clear had not happened
  // yet when the observer was selected; the disconnect still has to suppress it.
  CHECK(observer_calls == 0);
  // Tool dispatch belongs to the registry, so the result still reached the queue.
  REQUIRE(fixture.commands->try_pop());
}

TEST_CASE("disconnecting from inside on_finished reports false") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  std::shared_ptr<scry::detail::TurnRoute> route;
  std::size_t observed_size = 0;
  bool disconnect_reported = true;
  bool finished = false;
  route =
      fixture.route(225, {
                             .callbacks =
                                 scry::TurnCallbacks{
                                     .on_finished =
                                         [&route, &observed_size, &disconnect_reported,
                                          &finished, captured = std::string(64, 'z')](
                                             scry::Result<scry::Completion>) {
                                           finished = true;
                                           // The route is already finished here, so the
                                           // disconnect is a no-op rather than a
                                           // deferred clear.
                                           disconnect_reported = route->disconnect();
                                           observed_size = captured.size();
                                         },
                                 },
                         });
  pump.add_route(route);
  REQUIRE(fixture.events->push_terminal(completion_event(route->id()), 1024));

  CHECK(pump.update({}).callbacks_delivered == 1);

  CHECK(finished);
  CHECK_FALSE(disconnect_reported);
  CHECK(observed_size == 64);
}

TEST_CASE("a finished route releases its callback captures while its handle stays "
          "attached") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  auto captured = std::make_shared<int>(7);
  const std::weak_ptr<int> observed = captured;
  bool finished = false;
  const auto route = fixture.route(
      226,
      {
          .callbacks =
              scry::TurnCallbacks{
                  .on_finished =
                      [&finished, held = std::move(captured)](
                          scry::Result<scry::Completion>) { finished = *held == 7; },
              },
      });
  pump.add_route(route);
  REQUIRE(fixture.events->push_terminal(completion_event(route->id()), 1024));

  CHECK(pump.update({}).callbacks_delivered == 1);
  CHECK(finished);

  // The handle is still attached, so the route keeps its identity and status,
  // but a finished turn has no further use for the host's captures.
  CHECK(pump.find_route(route->id()));
  CHECK(route->finished());
  CHECK(observed.expired());
}

TEST_CASE("a route with no on_finished still delivers a text delta queued before its "
          "terminal event") {
  PumpFixture fixture;
  scry::detail::PumpState pump{fixture.events};
  auto captured = std::make_shared<int>(11);
  const std::weak_ptr<int> observed = captured;
  std::string streamed;
  const auto route =
      fixture.route(227, {
                             .callbacks =
                                 scry::TurnCallbacks{
                                     .on_text_delta =
                                         [&streamed, held = std::move(captured)](
                                             const std::string_view delta) {
                                           streamed.append(delta);
                                           CHECK(*held == 11);
                                         },
                                 },
                         });
  pump.add_route(route);
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = route->id(), .text = "streamed"}, 1024));
  REQUIRE(fixture.events->push_terminal(completion_event(route->id()), 1024));

  // The terminal lands with nothing observing it, so the route is finished
  // while the delta it queued earlier is still owed to the host.
  const auto terminal = pump.update({.max_callbacks = 0});
  CHECK(terminal.callbacks_delivered == 0);
  CHECK(route->finished());
  CHECK(streamed.empty());
  CHECK_FALSE(observed.expired());

  CHECK(pump.update({}).callbacks_delivered == 1);
  CHECK(streamed == "streamed");
  CHECK(observed.expired());
}

TEST_CASE("terminal routes are cleaned even when every update is out of time") {
  PumpFixture fixture;
  const auto now = std::chrono::steady_clock::time_point{};
  scry::detail::PumpState pump{fixture.events, [now] { return now; }};
  bool finished = false;
  const auto route = fixture.route(
      228,
      {
          .callbacks =
              scry::TurnCallbacks{
                  .on_finished =
                      [&finished](scry::Result<scry::Completion>) { finished = true; },
              },
      });
  pump.add_route(route);
  route->detach();
  REQUIRE(fixture.events->push_terminal(completion_event(route->id()), 1024));
  // A second queued event keeps ingestion from draining the queue, so the
  // deadline check after the first pop reports an exhausted budget.
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = scry::TurnId{.value = 999},
                                   .text = "orphan"},
      1024));

  const auto stats = pump.update({.time_budget = std::chrono::microseconds{0}});

  CHECK(stats.budget_exhausted);
  CHECK(stats.callbacks_delivered == 1);
  CHECK(finished);
  // Cleanup is a single pass over the routes, so it runs even here.
  CHECK_FALSE(pump.find_route(route->id()));
}
