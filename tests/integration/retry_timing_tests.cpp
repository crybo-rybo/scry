#include "runtime/test_access.hpp"
#include "runtime/worker.hpp"
#include "support/harness_test_support.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <scry/scry.hpp>
#include <stop_token>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using namespace scry::test_support;

namespace {

using scry::detail::MachineTimePoint;

// Replaces the worker's steady clock and its retry wait. Every wait records the
// deadline it was asked for and jumps the clock straight to it, so a nonzero
// backoff or a Retry-After is scheduled and observed exactly, in no wall-clock
// time at all. `deadlines()` being non-empty is itself the proof that the worker
// used this source rather than the real clock.
class FakeWorkerClock final {
public:
  // Far enough from the epoch that saturating deadline arithmetic behaves as it
  // does in production.
  static constexpr MachineTimePoint origin = MachineTimePoint{} + 1h;

  [[nodiscard]] MachineTimePoint now() const {
    const std::scoped_lock lock{mutex_};
    return now_;
  }

  [[nodiscard]] std::optional<scry::detail::WorkerCommand>
  wait_until(scry::detail::CommandQueue& commands, const std::stop_token&,
             const MachineTimePoint deadline) {
    {
      const std::scoped_lock lock{mutex_};
      deadlines_.push_back(deadline);
      now_ = std::max(now_, deadline);
    }
    // A command queued while the turn was waiting must still be consumed, which
    // is what the real wait_pop_until does when it wakes early.
    return commands.try_pop();
  }

  [[nodiscard]] std::vector<MachineTimePoint> deadlines() const {
    const std::scoped_lock lock{mutex_};
    return deadlines_;
  }

private:
  mutable std::mutex mutex_{};
  MachineTimePoint now_{origin};
  std::vector<MachineTimePoint> deadlines_{};
};

[[nodiscard]] scry::detail::WorkerTimeSource
time_source(const std::shared_ptr<FakeWorkerClock>& clock) {
  return {
      .now = [clock] { return clock->now(); },
      .wait_until =
          [clock](scry::detail::CommandQueue& commands, const std::stop_token& stopped,
                  const MachineTimePoint deadline) {
            return clock->wait_until(commands, stopped, deadline);
          },
  };
}

[[nodiscard]] scry::Config retry_config() {
  auto config = test_config();
  config.retry.max_attempts = 3;
  config.retry.initial_backoff = 250ms;
  config.retry.max_backoff = 10s;
  config.retry.max_elapsed = 30s;
  config.retry.jitter_ratio = 0.0;
  return config;
}

[[nodiscard]] scry::test::ScriptedExchange
retryable_failure(const scry::ErrorCategory category = scry::ErrorCategory::network,
                  const std::optional<std::chrono::milliseconds> retry_after = {}) {
  return {
      .result = std::unexpected(scry::Error{
          .category = category,
          .retryable = true,
          .message = "scripted retryable failure",
          .retry_after = retry_after,
      }),
  };
}

} // namespace

TEST_CASE("retry backoff is scheduled exactly from the observed failure") {
  auto clock = std::make_shared<FakeWorkerClock>();
  auto fake = std::make_unique<scry::test::FakeTransport>();
  fake->enqueue(retryable_failure());
  fake->enqueue(retryable_failure());
  fake->enqueue(scripted_exchange(anthropic_text_stream("done"), "request-backoff"));
  auto harness = scry::detail::HarnessTestAccess::create(
      retry_config(), provider(), std::move(fake), 0, time_source(clock));
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  const auto completion = harness->send_and_wait(*conversation, "retry twice");

  REQUIRE(completion);
  CHECK(completion->text == "done");
  CHECK(completion->attempt_count == 3);

  // Attempt 1 fails at the origin, so the first wake is origin + 250 ms. The
  // second failure is observed at that wake, and the second backoff doubles to
  // 500 ms, so the second wake is origin + 750 ms.
  const auto deadlines = clock->deadlines();
  REQUIRE(deadlines.size() == 2);
  CHECK(deadlines[0] == FakeWorkerClock::origin + 250ms);
  CHECK(deadlines[1] == FakeWorkerClock::origin + 750ms);
  CHECK(clock->now() == FakeWorkerClock::origin + 750ms);
}

TEST_CASE("Retry-After is honored end to end") {
  auto clock = std::make_shared<FakeWorkerClock>();
  auto fake = std::make_unique<scry::test::FakeTransport>();
  fake->enqueue(retryable_failure(scry::ErrorCategory::rate_limit, 7s));
  fake->enqueue(scripted_exchange(anthropic_text_stream("done"), "request-rate-limit"));
  auto harness = scry::detail::HarnessTestAccess::create(
      retry_config(), provider(), std::move(fake), 0, time_source(clock));
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  const auto started = std::chrono::steady_clock::now();
  const auto completion = harness->send_and_wait(*conversation, "respect Retry-After");
  const auto elapsed = std::chrono::steady_clock::now() - started;

  REQUIRE(completion);
  CHECK(completion->attempt_count == 2);

  // The provider's 7 s beats the 250 ms exponential backoff and stays under the
  // 10 s cap, so exactly one wake is scheduled at origin + 7 s.
  const auto deadlines = clock->deadlines();
  REQUIRE(deadlines.size() == 1);
  CHECK(deadlines[0] == FakeWorkerClock::origin + 7s);
  // On the real clock this turn could not finish in under seven seconds.
  CHECK(elapsed < 5s);
}

TEST_CASE("the elapsed-time cap ends retrying") {
  auto clock = std::make_shared<FakeWorkerClock>();
  auto config = retry_config();
  config.retry.max_attempts = 10;
  config.retry.initial_backoff = 800ms;
  config.retry.max_elapsed = 1s;
  auto fake = std::make_unique<scry::test::FakeTransport>();
  fake->enqueue(retryable_failure());
  fake->enqueue(retryable_failure());
  fake->enqueue(retryable_failure());
  auto* observer = fake.get();
  auto harness = scry::detail::HarnessTestAccess::create(
      config, provider(), std::move(fake), 0, time_source(clock));
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  const auto completion = harness->send_and_wait(*conversation, "exhaust the window");

  // Derived from src/machine/turn_machine.cpp, which owns the cap; the worker
  // only drives the wait.
  //
  // Attempt 1 fails at the origin. retry_is_allowed passes (request attempt 1 of
  // 10, and origin <= origin + max_elapsed). retry_delay gives the 800 ms
  // initial backoff, so the wake deadline is origin + 800 ms; that is still
  // within the origin + 1 s elapsed deadline, so a ScheduleRetryWake is emitted.
  //
  // RetryWake at origin + 800 ms is not past the elapsed deadline, so attempt 2
  // is issued. It fails at origin + 800 ms. retry_is_allowed still passes, but
  // retry_delay now doubles to 1600 ms, putting the wake at origin + 2400 ms.
  // `deadline > elapsed_deadline` therefore holds and on_event(AttemptFailed)
  // takes the finish_error path instead of scheduling a third attempt.
  //
  // So: two attempts, one scheduled wake, and the terminal error is the second
  // failure correlated by TurnMachine::correlate, which stamps attempt with
  // attempt_count_ (2) and retryable from the network category. The third
  // scripted failure is never consumed.
  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::network);
  CHECK(completion.error().retryable);
  CHECK(completion.error().attempt == 2);
  CHECK(observer->calls() == 2);
  CHECK(observer->remaining() == 1);

  const auto deadlines = clock->deadlines();
  REQUIRE(deadlines.size() == 1);
  CHECK(deadlines[0] == FakeWorkerClock::origin + 800ms);
  // No wake beyond the cap was ever requested.
  CHECK(deadlines[0] <= FakeWorkerClock::origin + config.retry.max_elapsed);
  CHECK(conversation->empty());
}
