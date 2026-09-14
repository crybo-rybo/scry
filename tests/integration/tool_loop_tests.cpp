#include "tool_loop_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace scry::test_support;

TEST_CASE("two-tool turn snapshots tools, resends results, and commits atomically") {
  auto fixture = make_harness_fixture(
      test_config(), {scripted_exchange(two_tool_stream, "tool-request"),
                      scripted_exchange(final_stream, "final-request")});
  auto* requests = fixture.transport;
  auto& harness = fixture.harness;
  auto& conversation = fixture.conversation;

  std::vector<std::string> timeline;
  std::vector<std::thread::id> callback_threads;
  std::vector<scry::ToolCall> observed;
  std::string first_arguments;
  std::string second_arguments;
  scry::TurnId first_context_turn{};
  std::string first_context_call_id;
  std::string first_context_tool_name;
  std::uint32_t first_context_round{};
  std::uint32_t first_context_index{};
  bool reentrant_registration_succeeded = false;

  // The contextual overload: the handler learns which call it is servicing
  // before the observer is told anything about it.
  REQUIRE(harness.tools().add(ordinal_tool_definition("first_tool"),
                              [&](const scry::ToolCallContext& context,
                                  scry::Json arguments) -> scry::Result<scry::Json> {
                                timeline.emplace_back("handler:first");
                                callback_threads.push_back(std::this_thread::get_id());
                                first_arguments = std::move(arguments.text);
                                // The context's views borrow from the call block being
                                // dispatched, so only owning copies survive past this
                                // frame.
                                first_context_turn = context.turn_id;
                                first_context_call_id = std::string{context.call_id};
                                first_context_tool_name =
                                    std::string{context.tool_name};
                                first_context_round = context.round;
                                first_context_index = context.index;
                                reentrant_registration_succeeded =
                                    static_cast<bool>(harness.tools().add(
                                        ordinal_tool_definition("reentrant_tool"),
                                        static_handler(R"({"handled":"reentrant"})")));
                                return scry::Json{.text = R"({"handled":"first"})"};
                              }));
  REQUIRE(harness.tools().add(ordinal_tool_definition("second_tool"),
                              [&](scry::Json arguments) -> scry::Result<scry::Json> {
                                timeline.emplace_back("handler:second");
                                callback_threads.push_back(std::this_thread::get_id());
                                second_arguments = std::move(arguments.text);
                                return scry::Json{.text = R"({"handled":"second"})"};
                              }));

  std::optional<scry::Completion> completion;
  auto turn_result =
      harness.send(conversation, "Run both tools",
                   {
                       .on_tool_call =
                           [&](const scry::ToolCall& call) {
                             timeline.push_back("observer:" + call.name);
                             callback_threads.push_back(std::this_thread::get_id());
                             observed.push_back(scry::ToolCall{
                                 .turn_id = call.turn_id,
                                 .id = call.id,
                                 .name = call.name,
                                 .arguments = call.arguments,
                                 .result = call.result,
                                 .is_error = call.is_error,
                                 .round = call.round,
                                 .index = call.index,
                             });
                           },
                       .on_finished =
                           [&](scry::Result<scry::Completion> finished) {
                             REQUIRE(finished);
                             completion = std::move(*finished);
                           },
                   });
  REQUIRE(turn_result);

  REQUIRE(harness.tools().add(ordinal_tool_definition("after_send_tool"),
                              static_handler(R"({"handled":"after-send"})")));

  CHECK(conversation.empty());
  REQUIRE(pump_one_until(harness, [&] { return timeline.size() >= 2; }));
  CHECK(timeline == std::vector<std::string>{"handler:first", "observer:first_tool"});
  CHECK(conversation.empty());
  REQUIRE(pump_one_until(harness, [&] { return timeline.size() >= 4; }));
  CHECK(timeline == std::vector<std::string>{
                        "handler:first",
                        "observer:first_tool",
                        "handler:second",
                        "observer:second_tool",
                    });
  CHECK(conversation.empty());
  REQUIRE(pump_until(harness, [&] { return completion.has_value(); }));

  CHECK(first_arguments == R"({"ordinal":1})");
  CHECK(second_arguments == R"({"ordinal":2})");

  REQUIRE(observed.size() == 2);
  CHECK(observed[0].id == "call-a");
  CHECK(observed[0].name == "first_tool");
  CHECK(observed[0].arguments.text == R"({"ordinal":1})");
  CHECK(observed[0].result.text == R"({"handled":"first"})");
  CHECK_FALSE(observed[0].is_error);
  CHECK(observed[0].round == 1);
  CHECK(observed[0].index == 0);
  // The handler and the observer describe the same call.
  CHECK(first_context_call_id == observed[0].id);
  CHECK(first_context_tool_name == observed[0].name);
  CHECK(first_context_turn == observed[0].turn_id);
  CHECK(first_context_round == observed[0].round);
  CHECK(first_context_index == observed[0].index);
  CHECK(observed[1].id == "call-b");
  CHECK(observed[1].name == "second_tool");
  CHECK(observed[1].arguments.text == R"({"ordinal":2})");
  CHECK(observed[1].result.text == R"({"handled":"second"})");
  CHECK_FALSE(observed[1].is_error);
  CHECK(observed[1].round == 1);
  CHECK(observed[1].index == 1);
  CHECK(reentrant_registration_succeeded);
  CHECK(harness.tools().size() == 4);
  for (const auto callback_thread : callback_threads) {
    CHECK(callback_thread == std::this_thread::get_id());
  }

  REQUIRE(completion);
  CHECK(completion->text == "all done");
  CHECK(completion->usage.input_tokens == 10);
  CHECK(completion->usage.output_tokens == 7);
  CHECK(completion->attempt_count == 2);
  CHECK(completion->finish_reason == scry::FinishReason::completed);
  CHECK(completion->tool_round_count == 1);
  CHECK(completion->tool_call_count == 2);
  CHECK(conversation.message_count() == 4);
  auto serialized = conversation.to_json();
  REQUIRE(serialized);
  CHECK(
      serialized->text ==
      R"({"messages":[{"content":[{"text":"Run both tools","type":"text"}],"role":"user"},{"content":[{"arguments":{"ordinal":1},"id":"call-a","name":"first_tool","type":"tool_call"},{"arguments":{"ordinal":2},"id":"call-b","name":"second_tool","type":"tool_call"}],"role":"assistant"},{"content":[{"is_error":false,"result":{"handled":"first"},"tool_call_id":"call-a","type":"tool_result"},{"is_error":false,"result":{"handled":"second"},"tool_call_id":"call-b","type":"tool_result"}],"role":"user"},{"content":[{"text":"all done","type":"text"}],"role":"assistant"}],"system_prompt":"","version":1})");

  const auto recorded = requests->requests();
  REQUIRE(recorded.size() == 2);
  const auto& initial_body = recorded[0].body;
  const auto& resend_body = recorded[1].body;
  CHECK(initial_body.find(R"("input_schema")") != std::string::npos);
  CHECK(initial_body.find(R"("properties":{"ordinal":{"type":"integer"}})") !=
        std::string::npos);
  CHECK(initial_body.find(R"("required":["ordinal"])") != std::string::npos);
  CHECK(initial_body.find(R"("additionalProperties":false)") != std::string::npos);
  require_order(initial_body, "first_tool", "second_tool");

  for (const auto* body : {&initial_body, &resend_body}) {
    CHECK(body->find("after_send_tool") == std::string::npos);
    CHECK(body->find("reentrant_tool") == std::string::npos);
  }
  CHECK(resend_body.find(R"("type":"tool_use")") != std::string::npos);
  CHECK(resend_body.find(R"("type":"tool_result")") != std::string::npos);
  require_order(resend_body, R"("id":"call-a")", R"("id":"call-b")");
  require_order(resend_body, R"("tool_use_id":"call-a")", R"("tool_use_id":"call-b")");
  require_order(resend_body, R"({\"handled\":\"first\"})",
                R"({\"handled\":\"second\"})");
}

TEST_CASE("a failing tool handler reaches the observer as an error result") {
  auto fixture = make_harness_fixture(
      test_config(), {scripted_exchange(two_tool_stream, "tool-request"),
                      scripted_exchange(final_stream, "final-request")});
  auto& harness = fixture.harness;
  auto& conversation = fixture.conversation;

  REQUIRE(harness.tools().add(ordinal_tool_definition("first_tool"),
                              [](scry::Json) -> scry::Result<scry::Json> {
                                return std::unexpected(scry::Error{
                                    .category = scry::ErrorCategory::tool,
                                    .message = "handler said no",
                                });
                              }));
  REQUIRE(harness.tools().add(ordinal_tool_definition("second_tool"),
                              static_handler(R"({"handled":"second"})")));

  std::vector<scry::ToolCall> observed;
  bool finished = false;
  auto turn_result =
      harness.send(conversation, "Run both tools",
                   {
                       .on_tool_call =
                           [&observed](const scry::ToolCall& call) {
                             observed.push_back(scry::ToolCall{
                                 .turn_id = call.turn_id,
                                 .id = call.id,
                                 .name = call.name,
                                 .arguments = call.arguments,
                                 .result = call.result,
                                 .is_error = call.is_error,
                             });
                           },
                       .on_finished =
                           [&finished](scry::Result<scry::Completion> outcome) {
                             finished = outcome.has_value();
                           },
                   });
  REQUIRE(turn_result);
  REQUIRE(pump_until(harness, [&finished] { return finished; }));

  REQUIRE(observed.size() == 2);
  CHECK(observed[0].name == "first_tool");
  CHECK(observed[0].is_error);
  CHECK(observed[0].result.text == R"({"error":"tool handler returned an error"})");
  CHECK(observed[1].name == "second_tool");
  CHECK_FALSE(observed[1].is_error);
  CHECK(observed[1].result.text == R"({"handled":"second"})");
}

TEST_CASE("a queued turn waits for the active turn's app-thread tool round") {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  auto* requests = fake.get();
  fake->enqueue(scripted_exchange(two_tool_stream, "tool-request"));
  fake->enqueue(scripted_exchange(final_stream, "first-final-request"));
  fake->enqueue(scripted_exchange(final_stream, "second-final-request"));
  auto harness = unwrap(scry::detail::HarnessTestAccess::create(
      test_config(), provider(), std::move(fake)));
  REQUIRE(harness.tools().add(ordinal_tool_definition("first_tool"),
                              static_handler(R"({"queue":1})")));
  REQUIRE(harness.tools().add(ordinal_tool_definition("second_tool"),
                              static_handler(R"({"queue":2})")));

  auto first_conversation = scry::Conversation::create();
  auto second_conversation = scry::Conversation::create();
  REQUIRE(first_conversation);
  REQUIRE(second_conversation);
  bool first_completed = false;
  bool second_completed = false;
  auto first_turn =
      harness.send(*first_conversation, "first queued turn",
                   {
                       .on_finished =
                           [&first_completed](scry::Result<scry::Completion> finished) {
                             first_completed = finished.has_value();
                           },
                   });
  auto second_turn = harness.send(
      *second_conversation, "second queued turn",
      {
          .on_finished =
              [&second_completed](scry::Result<scry::Completion> finished) {
                second_completed = finished.has_value();
              },
      });
  REQUIRE(first_turn);
  REQUIRE(second_turn);
  REQUIRE(pump_until(harness, [&] { return first_completed && second_completed; }));

  const auto recorded = requests->requests();
  REQUIRE(recorded.size() == 3);
  CHECK(recorded[0].body.find("first queued turn") != std::string::npos);
  CHECK(recorded[1].body.find("first queued turn") != std::string::npos);
  CHECK(recorded[1].body.find(R"("tool_use_id":"call-a")") != std::string::npos);
  CHECK(recorded[2].body.find("second queued turn") != std::string::npos);
  CHECK(recorded[2].body.find("call-a") == std::string::npos);
  CHECK(first_conversation->message_count() == 4);
  CHECK(second_conversation->message_count() == 2);
}

TEST_CASE("tool call batches fail atomically at the event queue boundary") {
  const std::string first_name(250, 'a');
  const std::string second_name(250, 'b');
  auto config = test_config();
  config.limits.max_queued_event_bytes_per_turn = 1024;
  auto fixture = make_harness_fixture(
      config, {scripted_exchange(large_tool_batch_stream(first_name, second_name),
                                 "tool-request")});
  auto* requests = fixture.transport;
  auto& harness = fixture.harness;
  auto& conversation = fixture.conversation;
  std::size_t handler_calls = 0;
  const auto handler = [&handler_calls](scry::Json) -> scry::Result<scry::Json> {
    ++handler_calls;
    return scry::Json{.text = "{}"};
  };
  REQUIRE(harness.tools().add(ordinal_tool_definition(first_name), handler));
  REQUIRE(harness.tools().add(ordinal_tool_definition(second_name), handler));
  std::optional<scry::Error> failure;
  auto turn = harness.send(conversation, "run an oversized batch",
                           {
                               .on_finished =
                                   [&failure](scry::Result<scry::Completion> finished) {
                                     if (!finished) {
                                       failure = std::move(finished.error());
                                     }
                                   },
                           });
  REQUIRE(turn);

  REQUIRE(pump_until(harness, [&failure] { return failure.has_value(); }));

  CHECK(failure->category == scry::ErrorCategory::resource_limit);
  CHECK(handler_calls == 0);
  CHECK(conversation.empty());
  CHECK(requests->requests().size() == 1);
}

// A tool result may be far larger than the per-turn event budget. The machine
// reserves the whole exchange against the Conversation limit before resending
// it, so the completion that hands that exchange to the host is not charged a
// second time at the queue boundary.
TEST_CASE("a tool result larger than the queue limit still completes when it fits "
          "the Conversation limit") {
  const std::string large_result = "\"" + std::string(3 * 1024 * 1024, 'x') + "\"";
  auto fixture = make_harness_fixture(
      test_config(),
      {scripted_exchange(anthropic_tool_stream(
                             {{.id = "call-1", .name = "large", .arguments = "{}"}}),
                         "tool-request"),
       scripted_exchange(anthropic_text_stream("done"), "final-request")});
  REQUIRE(fixture.harness.tools().add(
      scry::ToolDefinition{
          .name = "large",
          .description = "Returns a result larger than the event queue limit",
          .input_schema = {.text = "{}"},
      },
      [&large_result](scry::Json) -> scry::Result<scry::Json> {
        return scry::Json{.text = large_result};
      }));

  auto completion =
      fixture.harness.send_and_wait(fixture.conversation, "return a large result");

  REQUIRE(completion);
  CHECK(completion->text == "done");
  CHECK(fixture.transport->requests().size() == 2);
  CHECK(fixture.conversation.message_count() == 4);
}

TEST_CASE("Harness destruction stops a worker awaiting an app-thread tool result") {
  auto fixture = make_harness_fixture(
      test_config(), {scripted_exchange(two_tool_stream, "tool-request")});
  auto& harness = fixture.harness;
  auto& conversation = fixture.conversation;
  std::size_t handler_calls = 0;
  const auto handler = [&handler_calls](scry::Json) -> scry::Result<scry::Json> {
    ++handler_calls;
    return scry::Json{.text = "{}"};
  };
  REQUIRE(harness.tools().add(ordinal_tool_definition("first_tool"), handler));
  REQUIRE(harness.tools().add(ordinal_tool_definition("second_tool"), handler));
  std::size_t callbacks = 0;
  auto turn = harness.send(
      conversation, "destroy during app-thread tool wait",
      {
          .on_tool_call = [&callbacks](const scry::ToolCall&) { ++callbacks; },
          .on_finished = [&callbacks](scry::Result<scry::Completion>) { ++callbacks; },
      });
  REQUIRE(turn);

  constexpr std::size_t maximum_pumps = 100'000;
  bool tool_calls_pending = false;
  for (std::size_t pump = 0; pump < maximum_pumps && !tool_calls_pending; ++pump) {
    tool_calls_pending = harness.update({.max_callbacks = 0}).events_remaining == 2;
    std::this_thread::yield();
  }
  REQUIRE(tool_calls_pending);

  auto owned = std::optional<scry::Harness>{std::move(harness)};
  owned.reset();

  CHECK(handler_calls == 0);
  CHECK(callbacks == 0);
  CHECK(conversation.empty());
}

TEST_CASE("a tool handler that disconnects suppresses its own observer") {
  auto fixture = make_harness_fixture(
      test_config(),
      {scripted_exchange(
           anthropic_tool_stream(
               {{.id = "call-1", .name = "disconnect_me", .arguments = "{}"}}),
           "tool-request"),
       scripted_exchange(anthropic_text_stream("done"), "final-request")});
  auto& harness = fixture.harness;
  auto& conversation = fixture.conversation;

  std::optional<scry::Turn> turn;
  bool disconnect_reported = false;
  std::size_t observer_calls = 0;
  REQUIRE(harness.tools().add(
      scry::ToolDefinition{
          .name = "disconnect_me",
          .description = "Disconnects the turn from inside its own handler",
          .input_schema = {.text = R"({"type":"object"})"},
      },
      [&turn, &disconnect_reported](scry::Json) -> scry::Result<scry::Json> {
        disconnect_reported = turn->disconnect();
        return scry::Json{.text = R"({"handled":true})"};
      }));

  turn = unwrap(
      harness.send(conversation, "Disconnect from the handler",
                   {
                       .on_tool_call = [&observer_calls](
                                           const scry::ToolCall&) { ++observer_calls; },
                   }));
  REQUIRE(pump_until(harness, [&turn] { return turn->finished(); }));

  CHECK(disconnect_reported);
  CHECK(observer_calls == 0);
  // Disconnecting stops delivery only; the tool loop still ran to completion.
  CHECK(conversation.message_count() == 4);
}

TEST_CASE(
    "the per-turn call limit refuses the rest of a batch and the turn completes") {
  auto config = test_config();
  config.max_tool_calls_per_turn = 1;
  auto fixture =
      make_harness_fixture(config, {scripted_exchange(two_tool_stream, "tool-request"),
                                    scripted_exchange(final_stream, "final-request")});
  auto* requests = fixture.transport;
  auto& harness = fixture.harness;
  auto& conversation = fixture.conversation;

  std::size_t first_calls = 0;
  std::size_t second_calls = 0;
  REQUIRE(harness.tools().add(ordinal_tool_definition("first_tool"),
                              [&first_calls](scry::Json) -> scry::Result<scry::Json> {
                                ++first_calls;
                                return scry::Json{.text = R"({"handled":"first"})"};
                              }));
  REQUIRE(harness.tools().add(ordinal_tool_definition("second_tool"),
                              [&second_calls](scry::Json) -> scry::Result<scry::Json> {
                                ++second_calls;
                                return scry::Json{.text = R"({"handled":"second"})"};
                              }));

  std::vector<bool> observed_errors;
  std::optional<scry::Completion> completion;
  REQUIRE(harness.send(conversation, "Run both tools",
                       {
                           .on_tool_call =
                               [&observed_errors](const scry::ToolCall& call) {
                                 observed_errors.push_back(call.is_error);
                               },
                           .on_finished =
                               [&completion](scry::Result<scry::Completion> finished) {
                                 REQUIRE(finished);
                                 completion = std::move(*finished);
                               },
                       }));
  REQUIRE(pump_until(harness, [&completion] { return completion.has_value(); }));

  CHECK(first_calls == 1);
  CHECK(second_calls == 0);
  CHECK(observed_errors == std::vector<bool>{false, true});
  REQUIRE(completion);
  CHECK(completion->text == "all done");
  // The model asked for two calls, so both are counted; only one was refused.
  CHECK(completion->tool_call_count == 2);
  CHECK(completion->rejected_tool_call_count == 1);

  const auto recorded = requests->requests();
  REQUIRE(recorded.size() == 2);
  const auto& resend_body = recorded[1].body;
  require_order(
      resend_body, R"({\"handled\":\"first\"})",
      R"({\"error\":\"tool call limit for this turn reached; respond without calling tools\"})");
  CHECK(resend_body.find(R"({\"handled\":\"second\"})") == std::string::npos);
}

TEST_CASE("an admission hook's refusal text reaches the model on the wire") {
  auto fixture = make_harness_fixture(
      test_config(), {scripted_exchange(two_tool_stream, "tool-request"),
                      scripted_exchange(final_stream, "final-request")});
  auto* requests = fixture.transport;
  auto& harness = fixture.harness;
  auto& conversation = fixture.conversation;

  std::size_t second_calls = 0;
  REQUIRE(harness.tools().add(ordinal_tool_definition("first_tool"),
                              static_handler(R"({"handled":"first"})")));
  REQUIRE(harness.tools().add(ordinal_tool_definition("second_tool"),
                              [&second_calls](scry::Json) -> scry::Result<scry::Json> {
                                ++second_calls;
                                return scry::Json{.text = R"({"handled":"second"})"};
                              }));

  // The "accept this result, then stop" pattern: a host flag the hook consults,
  // so the turn completes and commits instead of rolling back.
  bool stop_dispatching = false;
  std::optional<scry::Completion> completion;
  REQUIRE(harness.send(
      conversation, "Run both tools",
      {
          .on_tool_request = [&stop_dispatching](const scry::ToolRequest& request)
              -> std::optional<scry::ToolRejection> {
            if (!stop_dispatching) {
              return std::nullopt;
            }
            return scry::ToolRejection{
                .model_message =
                    "budget spent before " + std::string{request.context.tool_name},
            };
          },
          .on_tool_call =
              [&stop_dispatching](const scry::ToolCall&) { stop_dispatching = true; },
          .on_finished =
              [&completion](scry::Result<scry::Completion> finished) {
                REQUIRE(finished);
                completion = std::move(*finished);
              },
      }));
  REQUIRE(pump_until(harness, [&completion] { return completion.has_value(); }));

  CHECK(second_calls == 0);
  REQUIRE(completion);
  CHECK(completion->tool_call_count == 2);
  CHECK(completion->rejected_tool_call_count == 1);
  CHECK(conversation.message_count() == 4);

  const auto recorded = requests->requests();
  REQUIRE(recorded.size() == 2);
  CHECK(recorded[1].body.find(R"({\"error\":\"budget spent before second_tool\"})") !=
        std::string::npos);
}

TEST_CASE("cancelling from the admission hook runs no handler and commits nothing") {
  // Only the tool round is scripted: a cancelled turn never asks for a final
  // response, so a second exchange would go unused.
  auto fixture = make_harness_fixture(
      test_config(), {scripted_exchange(two_tool_stream, "tool-request")});
  auto& harness = fixture.harness;
  auto& conversation = fixture.conversation;

  std::size_t handler_calls = 0;
  REQUIRE(harness.tools().add(ordinal_tool_definition("first_tool"),
                              [&handler_calls](scry::Json) -> scry::Result<scry::Json> {
                                ++handler_calls;
                                return scry::Json{.text = R"({"handled":"first"})"};
                              }));
  REQUIRE(harness.tools().add(ordinal_tool_definition("second_tool"),
                              static_handler(R"({"handled":"second"})")));

  const auto messages_before = conversation.message_count();
  std::size_t admissions = 0;
  std::size_t observed_calls = 0;
  std::optional<scry::Error> outcome;
  REQUIRE(
      harness.send(conversation, "Run both tools",
                   {
                       .on_tool_request = [&](const scry::ToolRequest& request)
                           -> std::optional<scry::ToolRejection> {
                         ++admissions;
                         // Admitting and cancelling at once: the host changed its mind
                         // about the whole turn, not about this one call.
                         harness.cancel(request.context.turn_id);
                         return std::nullopt;
                       },
                       .on_tool_call = [&observed_calls](
                                           const scry::ToolCall&) { ++observed_calls; },
                       .on_finished =
                           [&outcome](scry::Result<scry::Completion> finished) {
                             REQUIRE_FALSE(finished);
                             outcome = finished.error();
                           },
                   }));
  REQUIRE(pump_until(harness, [&outcome] { return outcome.has_value(); }));

  CHECK(admissions == 1);
  CHECK(handler_calls == 0);
  CHECK(observed_calls == 0);
  REQUIRE(outcome);
  CHECK(outcome->category == scry::ErrorCategory::cancelled);
  CHECK(conversation.message_count() == messages_before);
  CHECK_FALSE(conversation.busy());
}
