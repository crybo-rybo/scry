#include "runtime/test_access.hpp"
#include "support/harness_test_support.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <scry/scry.hpp>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using namespace scry::test_support;

namespace {

constexpr std::string_view answer = "edge answer";
const std::string completed_stream =
    anthropic_text_stream("edge answer", "msg_edge", {}, 3, 2);

constexpr std::string_view partial_stream = R"(event: message_start
data: {"type":"message_start","message":{"id":"msg_partial","type":"message","role":"assistant","content":[],"model":"test-model","stop_reason":null,"usage":{"input_tokens":3,"output_tokens":0}}}

event: content_block_start
data: {"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}}

event: content_block_delta
data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"partial"}}

)";

// These cases are about retrying, so unlike the shared single-attempt config
// this one leaves the retry budget at its default and only pins the jitter.
[[nodiscard]] scry::Config retrying_config() {
  auto config = scry::test_support::test_config();
  config.retry.max_attempts = scry::RetryPolicy{}.max_attempts;
  return config;
}

[[nodiscard]] scry::test::ScriptedExchange success() {
  return {
      .body_chunks = {std::string{completed_stream}},
      .result =
          scry::detail::TransportResult{
              .status_code = 200,
              .provider_request_id = "request-edge",
          },
  };
}

[[nodiscard]] scry::test::ScriptedExchange
transient_failure(const std::string_view message = "transient failure") {
  return {
      .result = std::unexpected(scry::Error{
          .category = scry::ErrorCategory::network,
          .retryable = true,
          .message = std::string{message},
      }),
  };
}

[[nodiscard]] scry::test::ScriptedExchange held_transient_failure() {
  auto exchange = transient_failure("held transient failure");
  exchange.hold = true;
  return exchange;
}

struct OverlapState {
  std::mutex mutex{};
  std::condition_variable_any changed{};
  std::size_t entered{};
  bool released{false};
};

// Two transports share one state block so a single rendezvous proves both
// Harness workers are inside a transfer at once; the shared fake cannot express
// a gate that spans two independent transport instances.
class OverlapTransport final : public scry::detail::Transport {
public:
  explicit OverlapTransport(std::shared_ptr<OverlapState> state)
      : state_(std::move(state)) {}

  [[nodiscard]] scry::Result<scry::detail::TransportResult>
  perform(const scry::detail::TransportRequest&, const std::stop_token stopped,
          const std::atomic<bool>& cancelled,
          scry::detail::BodyChunkSink& sink) override {
    {
      std::unique_lock lock{state_->mutex};
      ++state_->entered;
      if (state_->entered == 2) {
        state_->released = true;
        state_->changed.notify_all();
      }
      if (!state_->changed.wait(lock, stopped, [this] { return state_->released; })) {
        return std::unexpected(cancelled_error());
      }
    }
    if (cancelled.load(std::memory_order_acquire)) {
      return std::unexpected(cancelled_error());
    }
    if (auto status = sink(completed_stream); !status) {
      return std::unexpected(std::move(status.error()));
    }
    return scry::detail::TransportResult{
        .status_code = 200,
        .provider_request_id = "request-overlap",
    };
  }

private:
  [[nodiscard]] static scry::Error cancelled_error() {
    return {
        .category = scry::ErrorCategory::cancelled,
        .message = "overlap transport cancelled",
    };
  }

  std::shared_ptr<OverlapState> state_;
};

[[nodiscard]] scry::ToolDefinition tool(std::string name = "edge_tool",
                                        std::string schema = R"({"type":"object"})") {
  return {
      .name = std::move(name),
      .description = "edge test tool",
      .input_schema = {.text = std::move(schema)},
  };
}

} // namespace

TEST_CASE("a transient failure retries once and reports the successful attempt") {
  auto config = retrying_config();
  config.retry.initial_backoff = 0ms;
  config.retry.max_backoff = 0ms;
  auto fake = std::make_unique<scry::test::FakeTransport>();
  auto* observer = fake.get();
  fake->enqueue(transient_failure());
  fake->enqueue(success());
  auto harness =
      scry::detail::HarnessTestAccess::create(config, provider(), std::move(fake));
  auto conversation = scry::Conversation::create();
  REQUIRE(harness);
  REQUIRE(conversation);

  auto completion = harness->send_and_wait(*conversation, "retry me");

  REQUIRE(completion);
  CHECK(completion->text == "edge answer");
  CHECK(completion->attempt_count == 2);
  CHECK(completion->provider_request_id == "request-edge");
  CHECK(observer->requests().size() == 2);
  CHECK(observer->remaining() == 0);
  CHECK(conversation->message_count() == 2);
}

TEST_CASE("semantic output prevents a retry after a transient transport failure") {
  auto config = retrying_config();
  config.retry.initial_backoff = 0ms;
  config.retry.max_backoff = 0ms;
  auto fake = std::make_unique<scry::test::FakeTransport>();
  auto* observer = fake.get();
  auto failure = transient_failure("failure after output");
  failure.body_chunks.emplace_back(partial_stream);
  fake->enqueue(std::move(failure));
  fake->enqueue(success());
  auto harness =
      scry::detail::HarnessTestAccess::create(config, provider(), std::move(fake));
  auto conversation = scry::Conversation::create();
  REQUIRE(harness);
  REQUIRE(conversation);

  std::string streamed;
  std::optional<scry::Error> failure_result;
  auto turn = harness->send(
      *conversation, "do not retry",
      {
          .on_text_delta =
              [&streamed](const std::string_view text) { streamed.append(text); },
          .on_finished =
              [&failure_result](scry::Result<scry::Completion> finished) {
                if (!finished) {
                  failure_result = std::move(finished.error());
                }
              },
      });
  REQUIRE(turn);

  REQUIRE(
      pump_until(*harness, [&failure_result] { return failure_result.has_value(); }));
  CHECK(streamed == "partial");
  REQUIRE(failure_result);
  CHECK(failure_result->category == scry::ErrorCategory::network);
  CHECK(failure_result->retryable);
  CHECK(failure_result->attempt == 1);
  CHECK(observer->requests().size() == 1);
  CHECK(observer->remaining() == 1);
  CHECK(conversation->empty());
}

TEST_CASE("cancelling a pending retry wakes the worker without another attempt") {
  auto config = retrying_config();
  config.retry.initial_backoff = 30s;
  config.retry.max_backoff = 30s;
  config.retry.max_elapsed = 60s;
  auto fake = std::make_unique<scry::test::FakeTransport>();
  auto* observer = fake.get();
  // One scripted failure only: a second attempt would exhaust the script and
  // fail loudly instead of silently retrying.
  fake->enqueue(transient_failure("retry signal"));
  auto harness =
      scry::detail::HarnessTestAccess::create(config, provider(), std::move(fake));
  auto conversation = scry::Conversation::create();
  REQUIRE(harness);
  REQUIRE(conversation);

  bool cancelled = false;
  auto turn = harness->send(
      *conversation, "cancel retry",
      {
          .on_finished =
              [&cancelled](scry::Result<scry::Completion> finished) {
                cancelled = !finished &&
                            finished.error().category == scry::ErrorCategory::cancelled;
              },
      });
  REQUIRE(turn);
  observer->wait_for_call(1);

  CHECK(turn->cancel());
  REQUIRE(pump_until(*harness, [&cancelled] { return cancelled; }));
  CHECK_FALSE(turn->cancel());
  CHECK(observer->calls() == 1);
  CHECK(conversation->empty());
}

TEST_CASE("a queued turn command is consumed before a zero-backoff retry wakes") {
  auto config = test_config();
  config.retry.max_attempts = 2;
  config.retry.initial_backoff = 0ms;
  config.retry.max_backoff = 0ms;
  auto fake = std::make_unique<scry::test::FakeTransport>();
  auto* observer = fake.get();
  // The first attempt is held so the second turn's command is already queued
  // when the zero-backoff retry wait evaluates its predicate; the retry and the
  // second turn then consume the two successes.
  fake->enqueue(held_transient_failure());
  fake->enqueue(success());
  fake->enqueue(success());
  auto harness =
      scry::detail::HarnessTestAccess::create(config, provider(), std::move(fake));
  auto first_conversation = scry::Conversation::create();
  auto second_conversation = scry::Conversation::create();
  REQUIRE(harness);
  REQUIRE(first_conversation);
  REQUIRE(second_conversation);

  std::optional<scry::Completion> first_completion;
  std::optional<scry::Error> first_failure;
  auto first_turn =
      harness->send(*first_conversation, "retry after queued command",
                    {
                        .on_finished =
                            [&first_completion,
                             &first_failure](scry::Result<scry::Completion> finished) {
                              if (finished) {
                                first_completion = std::move(*finished);
                              } else {
                                first_failure = std::move(finished.error());
                              }
                            },
                    });
  REQUIRE(first_turn);
  observer->wait_for_call(1);

  std::optional<scry::Completion> second_completion;
  std::optional<scry::Error> second_failure;
  auto second_turn =
      harness->send(*second_conversation, "queued while retrying",
                    {
                        .on_finished =
                            [&second_completion,
                             &second_failure](scry::Result<scry::Completion> finished) {
                              if (finished) {
                                second_completion = std::move(*finished);
                              } else {
                                second_failure = std::move(finished.error());
                              }
                            },
                    });
  REQUIRE(second_turn);

  // The worker is still inside the held transfer, so the second SendTurnCommand
  // is queued when the zero-backoff retry wait evaluates its predicate.
  observer->release();

  REQUIRE(pump_until(*harness, [&] {
    return (first_completion || first_failure) && (second_completion || second_failure);
  }));
  REQUIRE_FALSE(first_failure);
  REQUIRE_FALSE(second_failure);
  REQUIRE(first_completion);
  REQUIRE(second_completion);
  CHECK(first_completion->attempt_count == 2);
  CHECK(second_completion->attempt_count == 1);
  CHECK(observer->calls() == 3);
  CHECK(first_conversation->message_count() == 2);
  CHECK(second_conversation->message_count() == 2);
}

TEST_CASE("a completion one byte over the Conversation limit is not committed") {
  constexpr std::string_view question = "limit";
  auto config = retrying_config();
  config.limits.max_conversation_bytes = question.size() + answer.size() - 1;
  auto fake = std::make_unique<scry::test::FakeTransport>();
  fake->enqueue(success());
  auto harness =
      scry::detail::HarnessTestAccess::create(config, provider(), std::move(fake));
  auto conversation = scry::Conversation::create();
  REQUIRE(harness);
  REQUIRE(conversation);

  auto completion = harness->send_and_wait(*conversation, std::string{question});

  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::resource_limit);
  CHECK(completion.error().attempt == 1);
  CHECK(completion.error().provider_request_id == "request-edge");
  CHECK(conversation->empty());
  CHECK(conversation->message_count() == 0);
}

TEST_CASE("post-completion cancellation is safe and idempotent") {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  fake->enqueue(success());
  auto harness = scry::detail::HarnessTestAccess::create(retrying_config(), provider(),
                                                         std::move(fake));
  auto conversation = scry::Conversation::create();
  REQUIRE(harness);
  REQUIRE(conversation);

  bool completed = false;
  std::size_t finished_count = 0;
  auto turn = harness->send(
      *conversation, "complete first",
      {
          .on_finished =
              [&completed, &finished_count](scry::Result<scry::Completion> finished) {
                completed = finished.has_value();
                ++finished_count;
              },
      });
  REQUIRE(turn);
  REQUIRE(pump_until(*harness, [&completed] { return completed; }));

  CHECK_FALSE(turn->cancel());
  for (std::size_t pump = 0; pump < 32; ++pump) {
    static_cast<void>(harness->update());
    std::this_thread::yield();
  }
  // The terminal contract is exactly once: a cancel after completion delivers
  // nothing further.
  CHECK(finished_count == 1);
  CHECK(conversation->message_count() == 2);
}

TEST_CASE("send-and-wait maps transport cancellation to a cancelled result") {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  fake->enqueue({
      .result = std::unexpected(scry::Error{
          .category = scry::ErrorCategory::cancelled,
          .message = "scripted cancellation",
      }),
  });
  auto harness = scry::detail::HarnessTestAccess::create(retrying_config(), provider(),
                                                         std::move(fake));
  auto conversation = scry::Conversation::create();
  REQUIRE(harness);
  REQUIRE(conversation);

  auto completion = harness->send_and_wait(*conversation, "cancelled");

  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::cancelled);
  CHECK(completion.error().turn_id.has_value());
  CHECK(conversation->empty());
}

TEST_CASE("callbacks may use public operations and nested update is diagnosed") {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  fake->enqueue(success());
  fake->enqueue(success());
  auto harness = scry::detail::HarnessTestAccess::create(retrying_config(), provider(),
                                                         std::move(fake));
  auto first_conversation = scry::Conversation::create();
  auto second_conversation = scry::Conversation::create();
  REQUIRE(harness);
  REQUIRE(first_conversation);
  REQUIRE(second_conversation);

  std::optional<scry::Turn> first;
  std::optional<scry::Turn> second;
  bool first_completed = false;
  bool second_completed = false;
  bool nested_update_rejected = false;
  bool registration_succeeded = false;
  bool nested_send_succeeded = false;
  bool terminal_cancel_idempotent = false;
  std::optional<scry::Error> nested_wait_error;
  auto first_result = harness->send(
      *first_conversation, "first",
      {
          .on_finished =
              [&](scry::Result<scry::Completion> finished) {
                first_completed = finished.has_value();
                // A reentrant update is rejected, and budget_exhausted is the
                // only signal of it.
                nested_update_rejected = harness->update().budget_exhausted;
                registration_succeeded = static_cast<bool>(
                    harness->tools().add(tool(), static_handler(R"({"ok":true})")));
                auto nested_wait =
                    harness->send_and_wait(*second_conversation, "blocking second");
                if (!nested_wait) {
                  nested_wait_error = nested_wait.error();
                }
                terminal_cancel_idempotent = !first->cancel();
                auto nested = harness->send(
                    *second_conversation, "second",
                    {
                        .on_finished =
                            [&second_completed](scry::Result<scry::Completion> done) {
                              second_completed = done.has_value();
                            },
                    });
                nested_send_succeeded = nested.has_value();
                if (nested) {
                  second.emplace(std::move(*nested));
                }
              },
      });
  REQUIRE(first_result);
  first.emplace(std::move(*first_result));

  REQUIRE(pump_until(*harness, [&] { return first_completed && second_completed; }));
  CHECK(nested_update_rejected);
  CHECK(registration_succeeded);
  REQUIRE(nested_wait_error);
  CHECK(nested_wait_error->category == scry::ErrorCategory::invalid_state);
  CHECK(terminal_cancel_idempotent);
  CHECK(nested_send_succeeded);
  CHECK(harness->tools().size() == 1);
  CHECK(first_conversation->message_count() == 2);
  CHECK(second_conversation->message_count() == 2);
}

TEST_CASE("two Harness workers can overlap independent transfers") {
  auto overlap = std::make_shared<OverlapState>();
  auto first = scry::detail::HarnessTestAccess::create(
      retrying_config(), provider(), std::make_unique<OverlapTransport>(overlap));
  auto second = scry::detail::HarnessTestAccess::create(
      retrying_config(), provider(), std::make_unique<OverlapTransport>(overlap));
  auto first_conversation = scry::Conversation::create();
  auto second_conversation = scry::Conversation::create();
  REQUIRE(first);
  REQUIRE(second);
  REQUIRE(first_conversation);
  REQUIRE(second_conversation);

  bool first_completed = false;
  bool second_completed = false;
  auto first_turn =
      first->send(*first_conversation, "first",
                  {
                      .on_finished =
                          [&first_completed](scry::Result<scry::Completion> finished) {
                            first_completed = finished.has_value();
                          },
                  });
  auto second_turn = second->send(
      *second_conversation, "second",
      {
          .on_finished =
              [&second_completed](scry::Result<scry::Completion> finished) {
                second_completed = finished.has_value();
              },
      });
  REQUIRE(first_turn);
  REQUIRE(second_turn);

  constexpr std::size_t maximum_pumps = 100'000;
  for (std::size_t pump = 0;
       pump < maximum_pumps && (!first_completed || !second_completed); ++pump) {
    static_cast<void>(first->update());
    static_cast<void>(second->update());
    std::this_thread::yield();
  }

  CHECK(first_completed);
  CHECK(second_completed);
  {
    const std::scoped_lock lock{overlap->mutex};
    CHECK(overlap->entered == 2);
  }
  CHECK(first_conversation->message_count() == 2);
  CHECK(second_conversation->message_count() == 2);
}

TEST_CASE("ToolRegistry rejects invalid and duplicate registrations") {
  auto harness = scry::detail::HarnessTestAccess::create(
      retrying_config(), provider(), std::make_unique<scry::test::FakeTransport>());
  REQUIRE(harness);
  auto& tools = harness->tools();

  auto status =
      tools.add(tool("", R"({"type":"object"})"), static_handler(R"({"ok":true})"));
  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::invalid_argument);

  status = tools.add(tool("missing_schema", ""), static_handler(R"({"ok":true})"));
  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::invalid_argument);

  status = tools.add(tool("missing_handler"), {});
  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::invalid_argument);

  REQUIRE(tools.add(tool(), static_handler(R"({"ok":true})")));
  status = tools.add(tool(), static_handler(R"({"ok":true})"));
  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::invalid_argument);
  CHECK(tools.size() == 1);
}

TEST_CASE("ToolRegistry reports registered names in registration order") {
  auto harness = scry::detail::HarnessTestAccess::create(
      retrying_config(), provider(), std::make_unique<scry::test::FakeTransport>());
  REQUIRE(harness);
  auto& tools = harness->tools();
  CHECK(tools.names().empty());
  CHECK_FALSE(tools.contains("edge_tool"));

  REQUIRE(tools.add(tool("zulu"), static_handler(R"({"ok":true})")));
  REQUIRE(tools.add(tool("alpha"), static_handler(R"({"ok":true})")));

  CHECK(tools.contains("zulu"));
  CHECK(tools.contains("alpha"));
  CHECK_FALSE(tools.contains("bravo"));
  CHECK_FALSE(tools.contains(""));
  CHECK_FALSE(tools.contains("zul"));

  const auto names = tools.names();
  REQUIRE(names.size() == 2);
  CHECK(names[0] == "zulu");
  CHECK(names[1] == "alpha");
  CHECK(names.size() == tools.size());
}
