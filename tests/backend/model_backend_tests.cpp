#include "core/backend.hpp"
#include "core/provider.hpp"
#include "runtime/test_access.hpp"
#include "support/harness_test_support.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <scry/scry.hpp>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace scry::test_support;
using namespace std::chrono_literals;

namespace {

using scry::detail::ModelBackend;
using scry::detail::ModelResponse;
using scry::detail::ProviderCompleted;
using scry::detail::ProviderEvent;
using scry::detail::ProviderEventSink;
using scry::detail::ProviderSemanticOutput;
using scry::detail::ProviderTextDelta;

// One attempt a ScriptedBackend plays back: the events it streams, then its
// outcome.
struct ScriptedAttempt {
  std::vector<ProviderEvent> events{};
  scry::Result<ModelResponse> outcome{};
};

// What a test can still observe once the Harness owns the backend. The worker
// writes it and the test thread reads it, so every member is atomic.
struct BackendProbe {
  std::atomic<std::size_t> attempts{0};
  std::atomic<bool> blocked{false};
  std::atomic<bool> observed_cancel{false};
  std::atomic<bool> observed_shutdown{false};
};

// Plays back scripted attempts through the worker's sink exactly as the seam's
// contract asks of a real backend: a failed sink Status ends the attempt.
class ScriptedBackend final : public ModelBackend {
public:
  ScriptedBackend(std::vector<ScriptedAttempt> attempts,
                  std::shared_ptr<BackendProbe> probe)
      : attempts_(std::move(attempts)), probe_(std::move(probe)) {}

  [[nodiscard]] scry::Result<ModelResponse>
  perform(const scry::Config&, const scry::detail::ModelRequest&, std::stop_token,
          const std::atomic<bool>&, ProviderEventSink& sink) override {
    const auto index = probe_->attempts.fetch_add(1);
    if (index >= attempts_.size()) {
      return std::unexpected(scry::Error{
          .category = scry::ErrorCategory::invalid_state,
          .message = "scripted backend has no queued attempt",
      });
    }
    auto& attempt = attempts_[index];
    for (auto& event : attempt.events) {
      if (auto status = sink(std::move(event)); !status) {
        return std::unexpected(std::move(status.error()));
      }
    }
    return std::move(attempt.outcome);
  }

private:
  std::vector<ScriptedAttempt> attempts_;
  std::shared_ptr<BackendProbe> probe_;
};

// Never produces output: it holds the attempt until the turn is cancelled or
// the Harness shuts down, then reports how it was released.
class BlockingBackend final : public ModelBackend {
public:
  explicit BlockingBackend(std::shared_ptr<BackendProbe> probe)
      : probe_(std::move(probe)) {}

  [[nodiscard]] scry::Result<ModelResponse> perform(const scry::Config&,
                                                    const scry::detail::ModelRequest&,
                                                    const std::stop_token shutdown,
                                                    const std::atomic<bool>& cancelled,
                                                    ProviderEventSink&) override {
    probe_->blocked.store(true);
    while (!shutdown.stop_requested() && !cancelled.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(1ms);
    }
    probe_->observed_shutdown.store(shutdown.stop_requested());
    probe_->observed_cancel.store(cancelled.load(std::memory_order_acquire));
    return std::unexpected(scry::Error{
        .category = scry::ErrorCategory::cancelled,
        .message = "blocking backend released",
    });
  }

private:
  std::shared_ptr<BackendProbe> probe_;
};

[[nodiscard]] ModelResponse text_response(std::string text,
                                          std::string request_id = "backend-request") {
  return ModelResponse{
      .content = {scry::TextBlock{.text = std::move(text)}},
      .finish_reason = scry::FinishReason::completed,
      .usage = {},
      .provider_request_id = std::move(request_id),
  };
}

[[nodiscard]] scry::Error retryable_network_failure() {
  return scry::Error{
      .category = scry::ErrorCategory::network,
      .retryable = true,
      .message = "scripted network failure",
  };
}

[[nodiscard]] scry::Config retrying_config() {
  auto config = test_config();
  config.retry.max_attempts = 3;
  config.retry.initial_backoff = 1ms;
  config.retry.max_backoff = 1ms;
  return config;
}

[[nodiscard]] scry::Harness backend_harness(scry::Config config,
                                            std::unique_ptr<ModelBackend> backend) {
  return unwrap(scry::detail::HarnessTestAccess::create_with_backend(
      std::move(config), std::move(backend)));
}

[[nodiscard]] scry::Harness scripted_harness(scry::Config config,
                                             std::vector<ScriptedAttempt> attempts,
                                             std::shared_ptr<BackendProbe> probe) {
  return backend_harness(std::move(config), std::make_unique<ScriptedBackend>(
                                                std::move(attempts), std::move(probe)));
}

} // namespace

TEST_CASE("streamed text deltas reach on_text_delta and the returned response "
          "completes the turn") {
  auto probe = std::make_shared<BackendProbe>();
  std::vector<ScriptedAttempt> attempts(1);
  attempts[0].events.emplace_back(ProviderSemanticOutput{});
  attempts[0].events.emplace_back(ProviderTextDelta{.text = "Hello "});
  attempts[0].events.emplace_back(ProviderTextDelta{.text = "world"});
  attempts[0].outcome = text_response("Hello world");
  auto harness = scripted_harness(test_config(), std::move(attempts), probe);
  auto conversation = unwrap(scry::Conversation::create());

  std::string streamed;
  std::optional<scry::Result<scry::Completion>> outcome;
  auto turn = unwrap(harness.send(
      conversation, "hi",
      scry::TurnCallbacks{
          .on_text_delta =
              [&streamed](const std::string_view text) { streamed += text; },
          .on_finished =
              [&outcome](scry::Result<scry::Completion> result) {
                outcome = std::move(result);
              },
      }));
  REQUIRE(pump_until(harness, [&outcome] { return outcome.has_value(); }));

  REQUIRE(*outcome);
  const auto& completion = **outcome;
  CHECK(streamed == "Hello world");
  CHECK(completion.text == "Hello world");
  CHECK(completion.attempt_count == 1);
  CHECK(completion.provider_request_id == "backend-request");
  CHECK(probe->attempts.load() == 1);
  CHECK(conversation.message_count() == 2);
}

TEST_CASE("the worker redacts the API key from a backend's request identifier") {
  auto probe = std::make_shared<BackendProbe>();
  std::vector<ScriptedAttempt> attempts(1);
  attempts[0].outcome = text_response("done", "id-sanitized-test-key");
  auto harness = scripted_harness(test_config(), std::move(attempts), probe);
  auto conversation = unwrap(scry::Conversation::create());

  const auto completion = harness.send_and_wait(conversation, "redact");

  REQUIRE(completion);
  CHECK(completion->provider_request_id.empty());
}

TEST_CASE("semantic output before a retryable failure ends retry eligibility") {
  auto probe = std::make_shared<BackendProbe>();
  std::vector<ScriptedAttempt> attempts(2);
  attempts[0].events.emplace_back(ProviderSemanticOutput{});
  attempts[0].outcome = std::unexpected(retryable_network_failure());
  attempts[1].outcome = text_response("must not be requested");
  auto harness = scripted_harness(retrying_config(), std::move(attempts), probe);
  auto conversation = unwrap(scry::Conversation::create());

  const auto completion = harness.send_and_wait(conversation, "no retry");

  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::network);
  CHECK(completion.error().message == "scripted network failure");
  CHECK(completion.error().attempt == 1);
  CHECK(probe->attempts.load() == 1);
  CHECK(conversation.empty());
}

TEST_CASE("a retryable failure before semantic output is retried") {
  auto probe = std::make_shared<BackendProbe>();
  std::vector<ScriptedAttempt> attempts(2);
  attempts[0].outcome = std::unexpected(retryable_network_failure());
  attempts[1].events.emplace_back(ProviderSemanticOutput{});
  attempts[1].events.emplace_back(ProviderTextDelta{.text = "second"});
  attempts[1].outcome = text_response("second");
  auto harness = scripted_harness(retrying_config(), std::move(attempts), probe);
  auto conversation = unwrap(scry::Conversation::create());

  const auto completion = harness.send_and_wait(conversation, "retry once");

  REQUIRE(completion);
  CHECK(completion->text == "second");
  CHECK(completion->attempt_count == 2);
  CHECK(probe->attempts.load() == 2);
}

TEST_CASE("a completion streamed through the event sink fails the turn with "
          "protocol") {
  auto probe = std::make_shared<BackendProbe>();
  std::vector<ScriptedAttempt> attempts(1);
  attempts[0].events.emplace_back(ProviderCompleted{.response = text_response("one")});
  attempts[0].events.emplace_back(ProviderCompleted{.response = text_response("two")});
  attempts[0].outcome = text_response("returned");
  auto harness = scripted_harness(retrying_config(), std::move(attempts), probe);
  auto conversation = unwrap(scry::Conversation::create());

  const auto completion = harness.send_and_wait(conversation, "two completions");

  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::protocol);
  CHECK(completion.error().message == "model backend streamed a completion event");
  // Protocol failures are never retried.
  CHECK(probe->attempts.load() == 1);
  CHECK(conversation.empty());
}

TEST_CASE("Turn::cancel releases a backend blocked in an attempt") {
  auto probe = std::make_shared<BackendProbe>();
  auto harness =
      backend_harness(test_config(), std::make_unique<BlockingBackend>(probe));
  auto conversation = unwrap(scry::Conversation::create());

  std::optional<scry::Result<scry::Completion>> outcome;
  auto turn = unwrap(harness.send(
      conversation, "block",
      scry::TurnCallbacks{
          .on_finished =
              [&outcome](scry::Result<scry::Completion> r) { outcome = std::move(r); },
      }));
  REQUIRE(pump_until_deadline(harness, [&probe] { return probe->blocked.load(); }, 5s));

  CHECK(turn.cancel());
  REQUIRE(pump_until_deadline(harness, [&outcome] { return outcome.has_value(); }, 5s));

  REQUIRE_FALSE(*outcome);
  CHECK(outcome->error().category == scry::ErrorCategory::cancelled);
  CHECK(probe->observed_cancel.load());
  CHECK_FALSE(probe->observed_shutdown.load());
  CHECK(conversation.empty());
}

TEST_CASE("Harness destruction releases a backend blocked in an attempt") {
  auto probe = std::make_shared<BackendProbe>();
  auto conversation = unwrap(scry::Conversation::create());
  {
    auto harness =
        backend_harness(test_config(), std::make_unique<BlockingBackend>(probe));
    auto turn = unwrap(harness.send(conversation, "block"));
    REQUIRE(
        pump_until_deadline(harness, [&probe] { return probe->blocked.load(); }, 5s));
    // Leaving the scope destroys the Harness, which must stop the worker and
    // join it while the backend is still inside perform().
  }

  CHECK(probe->observed_shutdown.load());
  CHECK_FALSE(probe->observed_cancel.load());
  CHECK(conversation.empty());
}
