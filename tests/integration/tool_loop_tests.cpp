#include "tool_loop_test_support.hpp"

#include <optional>

using namespace scry::test_support;

TEST_CASE("two-tool turn snapshots tools, resends results, and commits atomically") {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  auto* requests = fake.get();
  fake->enqueue(scripted_exchange(two_tool_stream, "tool-request"));
  fake->enqueue(scripted_exchange(final_stream, "final-request"));
  auto harness_result = scry::detail::HarnessTestAccess::create(
      test_config(), provider(), std::move(fake));
  REQUIRE(harness_result);
  auto harness = std::move(*harness_result);

  std::vector<std::string> timeline;
  std::vector<std::thread::id> callback_threads;
  std::string first_arguments;
  std::string second_arguments;
  bool reentrant_registration_succeeded = false;

  REQUIRE(harness.tools().add(tool_definition("first_tool"),
                              [&](scry::Json arguments) -> scry::Result<scry::Json> {
                                timeline.emplace_back("handler:first");
                                callback_threads.push_back(std::this_thread::get_id());
                                first_arguments = std::move(arguments.text);
                                reentrant_registration_succeeded =
                                    static_cast<bool>(harness.tools().add(
                                        tool_definition("reentrant_tool"),
                                        static_handler(R"({"handled":"reentrant"})")));
                                return scry::Json{.text = R"({"handled":"first"})"};
                              }));
  REQUIRE(harness.tools().add(tool_definition("second_tool"),
                              [&](scry::Json arguments) -> scry::Result<scry::Json> {
                                timeline.emplace_back("handler:second");
                                callback_threads.push_back(std::this_thread::get_id());
                                second_arguments = std::move(arguments.text);
                                return scry::Json{.text = R"({"handled":"second"})"};
                              }));

  auto conversation_result = scry::Conversation::create();
  REQUIRE(conversation_result);
  auto conversation = std::move(*conversation_result);
  auto turn_result = harness.send(conversation, "Run both tools");
  REQUIRE(turn_result);
  auto turn = std::move(*turn_result);

  REQUIRE(harness.tools().add(tool_definition("after_send_tool"),
                              static_handler(R"({"handled":"after-send"})")));

  std::optional<scry::Completion> completion;
  REQUIRE(turn.on_tool_call([&](const scry::ToolCall& call) {
    timeline.push_back("observer:" + call.name);
    callback_threads.push_back(std::this_thread::get_id());
  }));
  REQUIRE(
      turn.on_completion([&](const scry::Completion& value) { completion = value; }));

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
  CHECK(conversation.message_count() == 4);
  auto serialized = conversation.to_json();
  REQUIRE(serialized);
  CHECK(
      serialized->text ==
      R"({"messages":[{"content":[{"text":"Run both tools","type":"text"}],"role":"user"},{"content":[{"arguments":{"ordinal":1},"id":"call-a","name":"first_tool","type":"tool_call"},{"arguments":{"ordinal":2},"id":"call-b","name":"second_tool","type":"tool_call"}],"role":"assistant"},{"content":[{"is_error":false,"result":{"handled":"first"},"tool_call_id":"call-a","type":"tool_result"},{"is_error":false,"result":{"handled":"second"},"tool_call_id":"call-b","type":"tool_result"}],"role":"user"},{"content":[{"text":"all done","type":"text"}],"role":"assistant"}],"system_prompt":"","version":1})");

  REQUIRE(requests->requests().size() == 2);
  const auto& initial_body = requests->requests()[0].body;
  const auto& resend_body = requests->requests()[1].body;
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

TEST_CASE("a queued turn waits for the active turn's app-thread tool round") {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  auto* requests = fake.get();
  fake->enqueue(scripted_exchange(two_tool_stream, "tool-request"));
  fake->enqueue(scripted_exchange(final_stream, "first-final-request"));
  fake->enqueue(scripted_exchange(final_stream, "second-final-request"));
  auto harness_result = scry::detail::HarnessTestAccess::create(
      test_config(), provider(), std::move(fake));
  REQUIRE(harness_result);
  auto harness = std::move(*harness_result);
  REQUIRE(harness.tools().add(tool_definition("first_tool"),
                              static_handler(R"({"queue":1})")));
  REQUIRE(harness.tools().add(tool_definition("second_tool"),
                              static_handler(R"({"queue":2})")));

  auto first_conversation = scry::Conversation::create();
  auto second_conversation = scry::Conversation::create();
  REQUIRE(first_conversation);
  REQUIRE(second_conversation);
  auto first_turn = harness.send(*first_conversation, "first queued turn");
  auto second_turn = harness.send(*second_conversation, "second queued turn");
  REQUIRE(first_turn);
  REQUIRE(second_turn);

  bool first_completed = false;
  bool second_completed = false;
  REQUIRE(first_turn->on_completion(
      [&first_completed](const scry::Completion&) { first_completed = true; }));
  REQUIRE(second_turn->on_completion(
      [&second_completed](const scry::Completion&) { second_completed = true; }));
  REQUIRE(pump_until(harness, [&] { return first_completed && second_completed; }));

  REQUIRE(requests->requests().size() == 3);
  CHECK(requests->requests()[0].body.find("first queued turn") != std::string::npos);
  CHECK(requests->requests()[1].body.find("first queued turn") != std::string::npos);
  CHECK(requests->requests()[1].body.find(R"("tool_use_id":"call-a")") !=
        std::string::npos);
  CHECK(requests->requests()[2].body.find("second queued turn") != std::string::npos);
  CHECK(requests->requests()[2].body.find("call-a") == std::string::npos);
  CHECK(first_conversation->message_count() == 4);
  CHECK(second_conversation->message_count() == 2);
}

TEST_CASE("tool call batches fail atomically at the event queue boundary") {
  const std::string first_name(250, 'a');
  const std::string second_name(250, 'b');
  auto fake = std::make_unique<scry::test::FakeTransport>();
  auto* requests = fake.get();
  fake->enqueue(scripted_exchange(large_tool_batch_stream(first_name, second_name),
                                  "tool-request"));
  auto config = test_config();
  config.limits.max_queued_event_bytes_per_turn = 1024;
  auto harness_result =
      scry::detail::HarnessTestAccess::create(config, provider(), std::move(fake));
  REQUIRE(harness_result);
  auto harness = std::move(*harness_result);
  std::size_t handler_calls = 0;
  const auto handler = [&handler_calls](scry::Json) -> scry::Result<scry::Json> {
    ++handler_calls;
    return scry::Json{.text = "{}"};
  };
  REQUIRE(harness.tools().add(tool_definition(first_name), handler));
  REQUIRE(harness.tools().add(tool_definition(second_name), handler));
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);
  auto turn = harness.send(*conversation, "run an oversized batch");
  REQUIRE(turn);
  std::optional<scry::Error> failure;
  REQUIRE(turn->on_error([&failure](const scry::Error& error) { failure = error; }));

  REQUIRE(pump_until(harness, [&failure] { return failure.has_value(); }));

  CHECK(failure->category == scry::ErrorCategory::resource_limit);
  CHECK(handler_calls == 0);
  CHECK(conversation->empty());
  CHECK(requests->requests().size() == 1);
}

TEST_CASE("Harness destruction stops a worker awaiting an app-thread tool result") {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  fake->enqueue(scripted_exchange(two_tool_stream, "tool-request"));
  auto harness_result = scry::detail::HarnessTestAccess::create(
      test_config(), provider(), std::move(fake));
  REQUIRE(harness_result);
  auto harness = std::move(*harness_result);
  std::size_t handler_calls = 0;
  const auto handler = [&handler_calls](scry::Json) -> scry::Result<scry::Json> {
    ++handler_calls;
    return scry::Json{.text = "{}"};
  };
  REQUIRE(harness.tools().add(tool_definition("first_tool"), handler));
  REQUIRE(harness.tools().add(tool_definition("second_tool"), handler));
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);
  auto turn = harness.send(*conversation, "destroy during app-thread tool wait");
  REQUIRE(turn);
  std::size_t callbacks = 0;
  REQUIRE(turn->on_tool_call([&callbacks](const scry::ToolCall&) { ++callbacks; }));
  REQUIRE(turn->on_error([&callbacks](const scry::Error&) { ++callbacks; }));
  REQUIRE(turn->on_cancelled([&callbacks](const scry::Cancelled&) { ++callbacks; }));

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
  CHECK(conversation->empty());
}
