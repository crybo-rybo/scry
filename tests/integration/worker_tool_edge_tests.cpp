#include "tool_loop_test_support.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stop_token>

using namespace scry::test_support;
using namespace std::chrono_literals;

namespace {

class HeldRetryTransport final : public scry::detail::Transport {
public:
  [[nodiscard]] scry::Result<scry::detail::TransportResult>
  perform(const scry::detail::TransportRequest&, const std::stop_token stopped,
          const std::atomic<bool>& cancelled,
          scry::detail::BodyChunkSink& sink) override {
    std::size_t call_number{};
    {
      std::unique_lock lock{mutex_};
      call_number = ++calls_;
      changed_.notify_all();
      if (call_number == 1 &&
          !changed_.wait(lock, stopped, [this] { return first_call_released_; })) {
        return std::unexpected(cancelled_error());
      }
    }
    if (cancelled.load(std::memory_order_acquire)) {
      return std::unexpected(cancelled_error());
    }
    if (call_number == 1) {
      return std::unexpected(scry::Error{
          .category = scry::ErrorCategory::network,
          .message = "held transient failure",
          .retryable = true,
      });
    }
    if (auto status = sink(final_stream); !status) {
      return std::unexpected(std::move(status.error()));
    }
    return scry::detail::TransportResult{
        .status_code = 200,
        .provider_request_id = "request-held-retry",
    };
  }

  void wait_for_first_call() {
    std::unique_lock lock{mutex_};
    changed_.wait(lock, [this] { return calls_ != 0; });
  }

  void release_first_call() {
    {
      const std::scoped_lock lock{mutex_};
      first_call_released_ = true;
    }
    changed_.notify_all();
  }

  [[nodiscard]] std::size_t calls() const {
    const std::scoped_lock lock{mutex_};
    return calls_;
  }

private:
  [[nodiscard]] static scry::Error cancelled_error() {
    return {
        .category = scry::ErrorCategory::cancelled,
        .message = "held retry transport cancelled",
    };
  }

  mutable std::mutex mutex_{};
  std::condition_variable_any changed_{};
  std::size_t calls_{};
  bool first_call_released_{false};
};

} // namespace

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

TEST_CASE("a command queued during backoff is consumed before the retry wakes") {
  auto config = test_config();
  config.retry.max_attempts = 2;
  config.retry.initial_backoff = 0ms;
  config.retry.max_backoff = 0ms;
  auto transport = std::make_unique<HeldRetryTransport>();
  auto* observer = transport.get();
  auto harness_result =
      scry::detail::HarnessTestAccess::create(config, provider(), std::move(transport));
  REQUIRE(harness_result);
  auto harness = std::move(*harness_result);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  auto turn = harness.send(*conversation, "retry after queued command");
  REQUIRE(turn);
  std::optional<scry::Completion> completion;
  std::optional<scry::Error> failure;
  REQUIRE(turn->on_complete(
      [&completion](const scry::Completion& value) { completion = value; }));
  REQUIRE(turn->on_error([&failure](const scry::Error& error) { failure = error; }));
  observer->wait_for_first_call();

  // The worker is still inside the held transfer, so this command is already
  // queued when the zero-backoff retry wait evaluates its predicate.
  REQUIRE(harness.tools().add(tool_definition("queued_worker_tool"),
                              static_handler("{}"),
                              {.execution = scry::ToolExecution::worker_thread}));
  observer->release_first_call();

  REQUIRE(pump_until(harness, [&] { return completion || failure; }));
  REQUIRE_FALSE(failure);
  REQUIRE(completion);
  CHECK(completion->attempt_count == 2);
  CHECK(completion->text == "all done");
  CHECK(observer->calls() == 2);
  CHECK(harness.tools().size() == 1);
  CHECK(conversation->message_count() == 2);
}
