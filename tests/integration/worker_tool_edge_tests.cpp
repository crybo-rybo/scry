#include "tool_loop_test_support.hpp"

#include <atomic>
#include <optional>

using namespace scry::test_support;

TEST_CASE("worker tool result limits fail the turn before later calls run") {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  auto* requests = fake.get();
  fake->enqueue(scripted_exchange(two_tool_stream, "tool-request"));
  auto config = test_config();
  config.limits.max_tool_result_bytes = 1;
  auto harness_result =
      scry::detail::HarnessTestAccess::create(config, provider(), std::move(fake));
  REQUIRE(harness_result);
  auto harness = std::move(*harness_result);

  std::atomic_size_t first_calls{};
  std::atomic_size_t second_calls{};
  const auto worker_mode =
      scry::ToolRegistrationOptions{.execution = scry::ToolExecution::worker_thread};
  REQUIRE(harness.tools().add(
      tool_definition("first_tool"),
      [&first_calls](scry::Json) -> scry::Result<scry::Json> {
        first_calls.fetch_add(1, std::memory_order_relaxed);
        return scry::Json{.text = R"({"oversized":true})"};
      },
      worker_mode));
  REQUIRE(harness.tools().add(
      tool_definition("second_tool"),
      [&second_calls](scry::Json) -> scry::Result<scry::Json> {
        second_calls.fetch_add(1, std::memory_order_relaxed);
        return scry::Json{.text = "{}"};
      },
      worker_mode));

  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);
  auto turn = harness.send(*conversation, "Enforce the worker result limit");
  REQUIRE(turn);
  std::optional<scry::Error> failure;
  REQUIRE(turn->on_error([&failure](const scry::Error& error) { failure = error; }));

  REQUIRE(pump_until(harness, [&failure] { return failure.has_value(); }));

  CHECK(failure->category == scry::ErrorCategory::resource_limit);
  CHECK(first_calls.load(std::memory_order_relaxed) == 1);
  CHECK(second_calls.load(std::memory_order_relaxed) == 0);
  CHECK(conversation->empty());
  CHECK(requests->requests().size() == 1);
}

TEST_CASE("detached worker tools execute, resend, and commit without observers") {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  auto* requests = fake.get();
  fake->enqueue(scripted_exchange(two_tool_stream, "tool-request"));
  fake->enqueue(scripted_exchange(final_stream, "final-request"));
  auto harness_result = scry::detail::HarnessTestAccess::create(
      test_config(), provider(), std::move(fake));
  REQUIRE(harness_result);
  auto harness = std::move(*harness_result);

  std::atomic_size_t handler_calls{};
  const auto worker_mode =
      scry::ToolRegistrationOptions{.execution = scry::ToolExecution::worker_thread};
  REQUIRE(harness.tools().add(
      tool_definition("first_tool"),
      [&handler_calls](scry::Json) -> scry::Result<scry::Json> {
        handler_calls.fetch_add(1, std::memory_order_relaxed);
        return scry::Json{.text = R"({"handled":"first"})"};
      },
      worker_mode));
  REQUIRE(harness.tools().add(
      tool_definition("second_tool"),
      [&handler_calls](scry::Json) -> scry::Result<scry::Json> {
        handler_calls.fetch_add(1, std::memory_order_relaxed);
        return scry::Json{.text = R"({"handled":"second"})"};
      },
      worker_mode));

  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);
  {
    auto turn = harness.send(*conversation, "Run detached worker tools");
    REQUIRE(turn);
  }

  REQUIRE(pump_until(harness,
                     [&conversation] { return conversation->message_count() == 4; }));

  CHECK(handler_calls.load(std::memory_order_relaxed) == 2);
  REQUIRE(requests->requests().size() == 2);
  const auto& resend = requests->requests().back().body;
  require_order(resend, R"("tool_use_id":"call-a")", R"("tool_use_id":"call-b")");
  CHECK(resend.find(R"({\"handled\":\"first\"})") != std::string::npos);
  CHECK(resend.find(R"({\"handled\":\"second\"})") != std::string::npos);
}
