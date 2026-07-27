#include "tool_loop_test_support.hpp"

#include <condition_variable>
#include <mutex>
#include <stdexcept>

using namespace scry::test_support;

TEST_CASE("mixed worker and app tools preserve provider order and observer threads") {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  fake->enqueue(scripted_exchange(two_tool_stream, "tool-request"));
  fake->enqueue(scripted_exchange(final_stream, "final-request"));
  auto harness_result = scry::detail::HarnessTestAccess::create(
      test_config(), provider(), std::move(fake));
  REQUIRE(harness_result);
  auto harness = std::move(*harness_result);

  std::mutex trace_mutex;
  std::vector<std::string> timeline;
  std::vector<std::thread::id> handler_threads;
  std::vector<std::thread::id> observer_threads;
  const auto record_handler = [&](std::string event) {
    const std::scoped_lock lock{trace_mutex};
    timeline.push_back(std::move(event));
    handler_threads.push_back(std::this_thread::get_id());
  };
  REQUIRE(harness.tools().add(tool_definition("first_tool"),
                              [record_handler](scry::Json) -> scry::Result<scry::Json> {
                                record_handler("handler:first");
                                return scry::Json{.text = R"({"handled":"worker"})"};
                              },
                              {.execution = scry::ToolExecution::worker_thread}));
  REQUIRE(harness.tools().add(tool_definition("second_tool"),
                              [record_handler](scry::Json) -> scry::Result<scry::Json> {
                                record_handler("handler:second");
                                return scry::Json{.text = R"({"handled":"app"})"};
                              }));
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);
  auto turn = harness.send(*conversation, "Run a mixed batch");
  REQUIRE(turn);
  bool completed = false;
  REQUIRE(turn->on_tool_call([&](const scry::ToolCall& call) {
    const std::scoped_lock lock{trace_mutex};
    timeline.push_back("observer:" + call.name);
    observer_threads.push_back(std::this_thread::get_id());
  }));
  REQUIRE(
      turn->on_completion([&completed](const scry::Completion&) { completed = true; }));

  const auto update_thread = std::this_thread::get_id();
  REQUIRE(pump_until(harness, [&completed] { return completed; }));

  const std::scoped_lock lock{trace_mutex};
  CHECK(timeline == std::vector<std::string>{
                        "handler:first",
                        "observer:first_tool",
                        "handler:second",
                        "observer:second_tool",
                    });
  REQUIRE(handler_threads.size() == 2);
  CHECK(handler_threads[0] != update_thread);
  CHECK(handler_threads[1] == update_thread);
  REQUIRE(observer_threads.size() == 2);
  CHECK(observer_threads[0] == update_thread);
  CHECK(observer_threads[1] == update_thread);
  CHECK(conversation->message_count() == 4);
}

TEST_CASE("all-worker tools contain handler exceptions and preserve provider order") {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  auto* requests = fake.get();
  fake->enqueue(scripted_exchange(two_tool_stream, "tool-request"));
  fake->enqueue(scripted_exchange(final_stream, "final-request"));
  auto harness_result = scry::detail::HarnessTestAccess::create(
      test_config(), provider(), std::move(fake));
  REQUIRE(harness_result);
  auto harness = std::move(*harness_result);

  std::mutex timeline_mutex;
  std::vector<std::string> timeline;
  const auto worker_mode =
      scry::ToolRegistrationOptions{.execution = scry::ToolExecution::worker_thread};
  REQUIRE(harness.tools().add(
      tool_definition("first_tool"),
      [&](scry::Json) -> scry::Result<scry::Json> {
        const std::scoped_lock lock{timeline_mutex};
        timeline.emplace_back("handler:first");
        throw std::runtime_error{"contained worker failure"};
      },
      worker_mode));
  REQUIRE(harness.tools().add(
      tool_definition("second_tool"),
      [&](scry::Json) -> scry::Result<scry::Json> {
        const std::scoped_lock lock{timeline_mutex};
        timeline.emplace_back("handler:second");
        return scry::Json{.text = R"({"handled":"second"})"};
      },
      worker_mode));

  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);
  auto turn = harness.send(*conversation, "Run the worker batch");
  REQUIRE(turn);
  bool completed = false;
  REQUIRE(turn->on_tool_call([&](const scry::ToolCall& call) {
    const std::scoped_lock lock{timeline_mutex};
    timeline.push_back("observer:" + call.name);
  }));
  REQUIRE(
      turn->on_completion([&completed](const scry::Completion&) { completed = true; }));
  REQUIRE(pump_until(harness, [&completed] { return completed; }));

  const std::scoped_lock lock{timeline_mutex};
  CHECK(timeline == std::vector<std::string>{
                        "handler:first",
                        "observer:first_tool",
                        "handler:second",
                        "observer:second_tool",
                    });
  REQUIRE(requests->requests().size() == 2);
  const auto& resend = requests->requests().back().body;
  require_order(resend, R"("tool_use_id":"call-a")", R"("tool_use_id":"call-b")");
  CHECK(resend.find(R"("is_error":true)") != std::string::npos);
  CHECK(resend.find(R"({\"handled\":\"second\"})") != std::string::npos);
}

TEST_CASE("an accepted turn does not consult later worker registrations") {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  auto* requests = fake.get();
  fake->enqueue(scripted_exchange(
      large_tool_batch_stream("first_tool", "after_send_tool"), "tool-request"));
  fake->enqueue(scripted_exchange(final_stream, "final-request"));
  auto harness_result = scry::detail::HarnessTestAccess::create(
      test_config(), provider(), std::move(fake));
  REQUIRE(harness_result);
  auto harness = std::move(*harness_result);
  REQUIRE(harness.tools().add(tool_definition("first_tool"),
                              static_handler(R"({"handled":"first"})")));

  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);
  auto turn = harness.send(*conversation, "Keep the accepted snapshot");
  REQUIRE(turn);
  std::mutex counter_mutex;
  std::size_t later_handler_calls = 0;
  REQUIRE(harness.tools().add(tool_definition("after_send_tool"),
                              [&](scry::Json) -> scry::Result<scry::Json> {
                                const std::scoped_lock lock{counter_mutex};
                                ++later_handler_calls;
                                return scry::Json{.text = R"({"unexpected":true})"};
                              },
                              {.execution = scry::ToolExecution::worker_thread}));

  bool completed = false;
  REQUIRE(
      turn->on_completion([&completed](const scry::Completion&) { completed = true; }));
  REQUIRE(pump_until(harness, [&completed] { return completed; }));

  const std::scoped_lock lock{counter_mutex};
  CHECK(later_handler_calls == 0);
  REQUIRE(requests->requests().size() == 2);
  CHECK(requests->requests().front().body.find("after_send_tool") == std::string::npos);
  CHECK(requests->requests().back().body.find(
            R"({\"error\":\"model requested an unknown tool\"})") != std::string::npos);
}

TEST_CASE("cancellation during a worker handler suppresses its batch and result") {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  auto* requests = fake.get();
  fake->enqueue(scripted_exchange(two_tool_stream, "tool-request"));
  auto harness_result = scry::detail::HarnessTestAccess::create(
      test_config(), provider(), std::move(fake));
  REQUIRE(harness_result);
  auto harness = std::move(*harness_result);

  std::mutex handler_mutex;
  std::condition_variable handler_ready;
  bool started = false;
  bool release = false;
  std::size_t app_handler_calls = 0;
  REQUIRE(harness.tools().add(tool_definition("first_tool"),
                              [&](scry::Json) -> scry::Result<scry::Json> {
                                std::unique_lock lock{handler_mutex};
                                started = true;
                                handler_ready.notify_one();
                                handler_ready.wait(lock,
                                                   [&release] { return release; });
                                return scry::Json{.text = R"({"suppressed":true})"};
                              },
                              {.execution = scry::ToolExecution::worker_thread}));
  REQUIRE(
      harness.tools().add(tool_definition("second_tool"),
                          [&app_handler_calls](scry::Json) -> scry::Result<scry::Json> {
                            ++app_handler_calls;
                            return scry::Json{.text = R"({"unexpected":true})"};
                          }));

  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);
  auto turn = harness.send(*conversation, "Cancel the worker batch");
  REQUIRE(turn);
  std::size_t observer_calls = 0;
  bool cancelled = false;
  REQUIRE(turn->on_tool_call(
      [&observer_calls](const scry::ToolCall&) { ++observer_calls; }));
  REQUIRE(
      turn->on_cancelled([&cancelled](const scry::Cancelled&) { cancelled = true; }));
  REQUIRE(pump_until(harness, [&] {
    const std::scoped_lock lock{handler_mutex};
    return started;
  }));

  CHECK(turn->cancel());
  {
    const std::scoped_lock lock{handler_mutex};
    release = true;
  }
  handler_ready.notify_one();
  REQUIRE(pump_until(harness, [&cancelled] { return cancelled; }));

  CHECK(observer_calls == 0);
  CHECK(app_handler_calls == 0);
  CHECK(conversation->empty());
  CHECK(requests->requests().size() == 1);
}

TEST_CASE("a queued turn remains serialized while a worker handler is held") {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  auto* requests = fake.get();
  fake->enqueue(scripted_exchange(two_tool_stream, "tool-request"));
  fake->enqueue(scripted_exchange(final_stream, "first-final-request"));
  fake->enqueue(scripted_exchange(final_stream, "second-final-request"));
  auto harness_result = scry::detail::HarnessTestAccess::create(
      test_config(), provider(), std::move(fake));
  REQUIRE(harness_result);
  auto harness = std::move(*harness_result);

  std::mutex handler_mutex;
  std::condition_variable handler_state;
  bool started = false;
  bool release = false;
  REQUIRE(harness.tools().add(tool_definition("first_tool"),
                              [&](scry::Json) -> scry::Result<scry::Json> {
                                std::unique_lock lock{handler_mutex};
                                started = true;
                                handler_state.notify_all();
                                handler_state.wait(lock,
                                                   [&release] { return release; });
                                return scry::Json{.text = R"({"handled":"first"})"};
                              },
                              {.execution = scry::ToolExecution::worker_thread}));
  REQUIRE(harness.tools().add(tool_definition("second_tool"),
                              static_handler(R"({"handled":"second"})"),
                              {.execution = scry::ToolExecution::worker_thread}));

  auto first_conversation = scry::Conversation::create();
  auto second_conversation = scry::Conversation::create();
  REQUIRE(first_conversation);
  REQUIRE(second_conversation);
  auto first = harness.send(*first_conversation, "first queued turn");
  auto second = harness.send(*second_conversation, "second queued turn");
  REQUIRE(first);
  REQUIRE(second);
  bool first_completed = false;
  bool second_completed = false;
  REQUIRE(first->on_completion(
      [&first_completed](const scry::Completion&) { first_completed = true; }));
  REQUIRE(second->on_completion(
      [&second_completed](const scry::Completion&) { second_completed = true; }));
  REQUIRE(pump_until(harness, [&] {
    const std::scoped_lock lock{handler_mutex};
    return started;
  }));

  CHECK(requests->requests().size() == 1);
  CHECK_FALSE(first_completed);
  CHECK_FALSE(second_completed);
  {
    const std::scoped_lock lock{handler_mutex};
    release = true;
  }
  handler_state.notify_all();
  REQUIRE(pump_until(harness, [&] { return first_completed && second_completed; }));
  REQUIRE(requests->requests().size() == 3);
  CHECK(requests->requests()[2].body.find("second queued turn") != std::string::npos);
}

TEST_CASE("destruction joins a cooperating worker handler without callbacks") {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  fake->enqueue(scripted_exchange(two_tool_stream, "tool-request"));
  auto harness_result = scry::detail::HarnessTestAccess::create(
      test_config(), provider(), std::move(fake));
  REQUIRE(harness_result);
  auto harness = std::move(*harness_result);

  std::mutex state_mutex;
  std::condition_variable state_changed;
  bool handler_started = false;
  bool handler_release = false;
  bool handler_returned = false;
  REQUIRE(harness.tools().add(tool_definition("first_tool"),
                              [&](scry::Json) -> scry::Result<scry::Json> {
                                std::unique_lock lock{state_mutex};
                                handler_started = true;
                                state_changed.notify_all();
                                state_changed.wait(lock, [&handler_release] {
                                  return handler_release;
                                });
                                handler_returned = true;
                                return scry::Json{.text = R"({"handled":"first"})"};
                              },
                              {.execution = scry::ToolExecution::worker_thread}));
  REQUIRE(harness.tools().add(tool_definition("second_tool"),
                              static_handler(R"({"unexpected":true})")));

  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);
  auto turn = harness.send(*conversation, "Destroy while the worker is held");
  REQUIRE(turn);
  std::size_t callbacks = 0;
  REQUIRE(turn->on_tool_call([&callbacks](const scry::ToolCall&) { ++callbacks; }));
  REQUIRE(turn->on_error([&callbacks](const scry::Error&) { ++callbacks; }));
  REQUIRE(turn->on_cancelled([&callbacks](const scry::Cancelled&) { ++callbacks; }));
  REQUIRE(pump_until(harness, [&] {
    const std::scoped_lock lock{state_mutex};
    return handler_started;
  }));

  bool teardown_started = false;
  bool teardown_finished = false;
  auto owned = std::optional<scry::Harness>{std::move(harness)};
  std::jthread teardown{[owned = std::move(owned), &state_mutex, &state_changed,
                         &teardown_started, &teardown_finished]() mutable {
    {
      const std::scoped_lock lock{state_mutex};
      teardown_started = true;
    }
    state_changed.notify_all();
    owned.reset();
    {
      const std::scoped_lock lock{state_mutex};
      teardown_finished = true;
    }
    state_changed.notify_all();
  }};
  {
    std::unique_lock lock{state_mutex};
    state_changed.wait(lock, [&teardown_started] { return teardown_started; });
    CHECK_FALSE(teardown_finished);
    handler_release = true;
  }
  state_changed.notify_all();
  teardown.join();

  const std::scoped_lock lock{state_mutex};
  CHECK(handler_returned);
  CHECK(teardown_finished);
  CHECK(callbacks == 0);
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
  REQUIRE(first_turn->on_completion(
      [&](const scry::Completion&) { first_completed = true; }));
  REQUIRE(second_turn->on_completion(
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
