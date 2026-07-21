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
