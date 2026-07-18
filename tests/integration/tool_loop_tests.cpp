#include "core/provider.hpp"
#include "runtime/test_access.hpp"
#include "support/transport/fake_transport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <memory>
#include <optional>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view two_tool_stream = R"(event: message_start
data: {"type":"message_start","message":{"id":"msg_tools","type":"message","role":"assistant","content":[],"model":"test-model","stop_reason":null,"usage":{"input_tokens":3,"output_tokens":0}}}

event: content_block_start
data: {"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"call-a","name":"first_tool","input":{}}}

event: content_block_delta
data: {"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{\"ordinal\":1}"}}

event: content_block_stop
data: {"type":"content_block_stop","index":0}

event: content_block_start
data: {"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"call-b","name":"second_tool","input":{}}}

event: content_block_delta
data: {"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"ordinal\":2}"}}

event: content_block_stop
data: {"type":"content_block_stop","index":1}

event: message_delta
data: {"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":2}}

event: message_stop
data: {"type":"message_stop"}

)";

constexpr std::string_view final_stream = R"(event: message_start
data: {"type":"message_start","message":{"id":"msg_final","type":"message","role":"assistant","content":[],"model":"test-model","stop_reason":null,"usage":{"input_tokens":7,"output_tokens":0}}}

event: content_block_start
data: {"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}}

event: content_block_delta
data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"all done"}}

event: content_block_stop
data: {"type":"content_block_stop","index":0}

event: message_delta
data: {"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":5}}

event: message_stop
data: {"type":"message_stop"}

)";

[[nodiscard]] scry::Config test_config() {
  auto config = scry::Config{
      .base_url = "http://127.0.0.1:1",
      .api_key = "sanitized-test-key",
      .model = "test-model",
  };
  config.retry.max_attempts = 1;
  config.retry.jitter_ratio = 0.0;
  return config;
}

[[nodiscard]] std::unique_ptr<scry::detail::ProviderAdapter> provider() {
  auto result = scry::detail::make_provider_adapter(scry::ProviderDialect::anthropic);
  REQUIRE(result);
  return std::move(*result);
}

[[nodiscard]] scry::test::ScriptedExchange
scripted_exchange(const std::string_view stream, std::string request_id) {
  return {
      .body_chunks = {std::string{stream}},
      .result =
          scry::detail::TransportResult{
              .status_code = 200,
              .provider_request_id = std::move(request_id),
          },
  };
}

[[nodiscard]] scry::ToolDefinition tool_definition(std::string name) {
  return {
      .name = std::move(name),
      .description = "Accepts an explicit ordinal",
      .input_schema =
          {
              .text =
                  R"({"type":"object","properties":{"ordinal":{"type":"integer"}},"required":["ordinal"],"additionalProperties":false})",
          },
  };
}

[[nodiscard]] scry::ToolHandler static_handler(std::string result) {
  return [result = std::move(result)](scry::Json) -> scry::Result<scry::Json> {
    return scry::Json{.text = result};
  };
}

[[nodiscard]] std::string tool_block_events(const std::size_t index,
                                            const std::string_view id,
                                            const std::string_view name) {
  return "event: content_block_start\n"
         "data: {\"type\":\"content_block_start\",\"index\":" +
         std::to_string(index) + ",\"content_block\":{\"type\":\"tool_use\",\"id\":\"" +
         std::string{id} + "\",\"name\":\"" + std::string{name} +
         "\",\"input\":{}}}\n\n"
         "event: content_block_stop\n"
         "data: {\"type\":\"content_block_stop\",\"index\":" +
         std::to_string(index) + "}\n\n";
}

[[nodiscard]] std::string large_tool_batch_stream(const std::string_view first,
                                                  const std::string_view second) {
  return std::string{R"(event: message_start
data: {"type":"message_start","message":{"id":"msg_tools","type":"message","role":"assistant","content":[],"stop_reason":null}}

)"} + tool_block_events(0, "call-a", first) +
         tool_block_events(1, "call-b", second) + R"(event: message_delta
data: {"type":"message_delta","delta":{"stop_reason":"tool_use"}}

event: message_stop
data: {"type":"message_stop"}

)";
}

template <typename Predicate>
[[nodiscard]] bool pump_until(scry::Harness& harness, Predicate&& predicate) {
  constexpr std::size_t maximum_pumps = 100'000;
  for (std::size_t pump = 0; pump < maximum_pumps; ++pump) {
    static_cast<void>(harness.update());
    if (std::forward<Predicate>(predicate)()) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

template <typename Predicate>
[[nodiscard]] bool pump_one_until(scry::Harness& harness, Predicate&& predicate) {
  constexpr std::size_t maximum_pumps = 100'000;
  for (std::size_t pump = 0; pump < maximum_pumps; ++pump) {
    static_cast<void>(harness.update({.max_callbacks = 1}));
    if (std::forward<Predicate>(predicate)()) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

void require_order(const std::string& text, const std::string_view first,
                   const std::string_view second) {
  const auto first_position = text.find(first);
  const auto second_position = text.find(second);
  REQUIRE(first_position != std::string::npos);
  REQUIRE(second_position != std::string::npos);
  CHECK(first_position < second_position);
}

} // namespace

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
  REQUIRE(turn.on_complete([&](const scry::Completion& value) { completion = value; }));

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

TEST_CASE("a queued turn waits for the active turn's tool round") {
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
  REQUIRE(first_turn->on_complete(
      [&](const scry::Completion&) { first_completed = true; }));
  REQUIRE(second_turn->on_complete(
      [&](const scry::Completion&) { second_completed = true; }));
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

TEST_CASE("cancellation before a queued tool call suppresses its handler") {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  auto* requests = fake.get();
  fake->enqueue(scripted_exchange(two_tool_stream, "tool-request"));
  auto harness_result = scry::detail::HarnessTestAccess::create(
      test_config(), provider(), std::move(fake));
  REQUIRE(harness_result);
  auto harness = std::move(*harness_result);

  std::optional<scry::Turn> turn;
  std::size_t first_handler_calls = 0;
  std::size_t second_handler_calls = 0;
  std::size_t observer_calls = 0;
  bool cancelled_from_handler = false;
  REQUIRE(harness.tools().add(
      tool_definition("first_tool"), [&](scry::Json) -> scry::Result<scry::Json> {
        ++first_handler_calls;
        cancelled_from_handler = turn.has_value() && turn->cancel();
        return scry::Json{.text = R"({"cancelled":true})"};
      }));
  REQUIRE(harness.tools().add(tool_definition("second_tool"),
                              [&](scry::Json) -> scry::Result<scry::Json> {
                                ++second_handler_calls;
                                return scry::Json{.text = R"({"unexpected":true})"};
                              }));

  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);
  auto accepted = harness.send(*conversation, "cancel after the first tool");
  REQUIRE(accepted);
  turn.emplace(std::move(*accepted));
  bool cancelled = false;
  REQUIRE(turn->on_tool_call([&](const scry::ToolCall&) { ++observer_calls; }));
  REQUIRE(turn->on_cancelled([&](const scry::Cancelled&) { cancelled = true; }));

  REQUIRE(pump_until(harness, [&] { return cancelled; }));
  CHECK(cancelled_from_handler);
  CHECK(first_handler_calls == 1);
  CHECK(second_handler_calls == 0);
  CHECK(observer_calls == 0);
  CHECK(conversation->empty());
  CHECK(requests->requests().size() == 1);
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
  REQUIRE(harness.tools().add(tool_definition(first_name),
                              [&handler_calls](scry::Json) -> scry::Result<scry::Json> {
                                ++handler_calls;
                                return scry::Json{.text = "{}"};
                              }));
  REQUIRE(harness.tools().add(tool_definition(second_name),
                              [&handler_calls](scry::Json) -> scry::Result<scry::Json> {
                                ++handler_calls;
                                return scry::Json{.text = "{}"};
                              }));
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
