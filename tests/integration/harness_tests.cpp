#include "core/provider.hpp"
#include "runtime/test_access.hpp"
#include "support/transport/fake_transport.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <scry/scry.hpp>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view anthropic_stream = R"(event: message_start
data: {"type":"message_start","message":{"id":"msg_integration","type":"message","role":"assistant","content":[],"model":"test-model","stop_reason":null,"usage":{"input_tokens":2,"output_tokens":0}}}

event: content_block_start
data: {"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}}

event: content_block_delta
data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hello "}}

event: content_block_delta
data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"runtime."}}

event: content_block_stop
data: {"type":"content_block_stop","index":0}

event: message_delta
data: {"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":2}}

event: message_stop
data: {"type":"message_stop"}

)";

[[nodiscard]] scry::Config test_config() {
  return {
      .base_url = "http://127.0.0.1:1",
      .api_key = "sanitized-test-key",
      .model = "test-model",
  };
}

[[nodiscard]] std::unique_ptr<scry::detail::ProviderAdapter> provider() {
  auto result = scry::detail::make_provider_adapter(scry::ProviderDialect::anthropic);
  REQUIRE(result);
  return std::move(*result);
}

[[nodiscard]] scry::test::ScriptedExchange successful_exchange() {
  return {
      .body_chunks = {std::string{anthropic_stream}},
      .result =
          scry::detail::TransportResult{
              .status_code = 200,
              .provider_request_id = "request-integration",
          },
  };
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

class ControllableTransport final : public scry::detail::Transport {
public:
  [[nodiscard]] scry::Result<scry::detail::TransportResult>
  perform(const scry::detail::TransportRequest& request, const std::stop_token stopped,
          const std::atomic<bool>& cancelled,
          scry::detail::BodyChunkSink& sink) override {
    {
      std::unique_lock lock{mutex_};
      ++calls_;
      requests_.push_back(request.body);
      entered_ = true;
      changed_.notify_all();
      if (!changed_.wait(lock, stopped, [this] { return released_; })) {
        return std::unexpected(cancelled_error());
      }
    }
    if (stopped.stop_requested() || cancelled.load(std::memory_order_acquire)) {
      return std::unexpected(cancelled_error());
    }
    if (auto status = sink(anthropic_stream); !status) {
      return std::unexpected(std::move(status.error()));
    }
    return scry::detail::TransportResult{
        .status_code = 200,
        .provider_request_id = "controlled-request",
    };
  }

  void wait_until_entered() {
    std::unique_lock lock{mutex_};
    changed_.wait(lock, [this] { return entered_; });
  }

  void release() {
    {
      const std::scoped_lock lock{mutex_};
      released_ = true;
    }
    changed_.notify_all();
  }

  [[nodiscard]] std::size_t calls() const {
    const std::scoped_lock lock{mutex_};
    return calls_;
  }

  [[nodiscard]] std::vector<std::string> requests() const {
    const std::scoped_lock lock{mutex_};
    return requests_;
  }

private:
  [[nodiscard]] static scry::Error cancelled_error() {
    return {
        .category = scry::ErrorCategory::cancelled,
        .message = "controlled transport stopped",
    };
  }

  mutable std::mutex mutex_{};
  std::condition_variable_any changed_{};
  std::size_t calls_{};
  std::vector<std::string> requests_{};
  bool entered_{false};
  bool released_{false};
};

} // namespace

TEST_CASE("public async path streams and commits only inside update") {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  auto* fake_observer = fake.get();
  fake->enqueue(successful_exchange());
  auto harness_result = scry::detail::HarnessTestAccess::create(
      test_config(), provider(), std::move(fake));
  REQUIRE(harness_result);
  auto harness = std::move(*harness_result);
  auto conversation_result = scry::Conversation::create();
  REQUIRE(conversation_result);
  auto conversation = std::move(*conversation_result);

  auto turn_result = harness.send(conversation, "Question");
  REQUIRE(turn_result);
  auto turn = std::move(*turn_result);
  std::string streamed;
  std::string completed;
  std::thread::id callback_thread{};
  REQUIRE(turn.on_text_delta(
      [&streamed](const std::string_view text) { streamed.append(text); }));
  REQUIRE(
      turn.on_completion([&completed, &callback_thread](const scry::Completion& value) {
        completed = value.text;
        callback_thread = std::this_thread::get_id();
      }));

  CHECK(completed.empty());
  REQUIRE(pump_until(harness, [&completed] { return !completed.empty(); }));
  CHECK(streamed == "Hello runtime.");
  CHECK(completed == "Hello runtime.");
  CHECK(callback_thread == std::this_thread::get_id());
  CHECK(conversation.message_count() == 2);
  REQUIRE(fake_observer->requests().size() == 1);
  CHECK(fake_observer->requests().front().body.find("Question") != std::string::npos);
  CHECK(fake_observer->requests().front().body.find("sanitized-test-key") ==
        std::string::npos);
}

TEST_CASE("send-and-wait layers over the async path") {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  fake->enqueue(successful_exchange());
  auto harness_result = scry::detail::HarnessTestAccess::create(
      test_config(), provider(), std::move(fake));
  REQUIRE(harness_result);
  auto conversation_result = scry::Conversation::create();
  REQUIRE(conversation_result);

  auto completion = harness_result->send_and_wait(*conversation_result, "Question");
  REQUIRE(completion);
  CHECK(completion->text == "Hello runtime.");
  CHECK(completion->finish_reason == scry::FinishReason::completed);
  CHECK(completion->usage.input_tokens == 2);
  CHECK(completion->usage.output_tokens == 2);
  CHECK(completion->attempt_count == 1);
  CHECK(completion->provider_request_id == "request-integration");
  CHECK(conversation_result->message_count() == 2);
}

TEST_CASE("accepted failure uses one async error channel and commits nothing") {
  auto config = test_config();
  config.retry.max_attempts = 1;
  auto fake = std::make_unique<scry::test::FakeTransport>();
  fake->enqueue({
      .result = std::unexpected(scry::Error{
          .category = scry::ErrorCategory::network,
          .message = "scripted failure",
          .retryable = true,
      }),
  });
  auto harness_result =
      scry::detail::HarnessTestAccess::create(config, provider(), std::move(fake));
  REQUIRE(harness_result);
  auto conversation_result = scry::Conversation::create();
  REQUIRE(conversation_result);

  auto completion = harness_result->send_and_wait(*conversation_result, "Question");
  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::network);
  CHECK(completion.error().turn_id.has_value());
  CHECK(completion.error().attempt == 1);
  CHECK(conversation_result->empty());
}

TEST_CASE("busy conversations and queued cancellation issue no second transfer") {
  auto config = test_config();
  config.limits.max_pending_turns = 2;
  auto transport = std::make_unique<ControllableTransport>();
  auto* transport_observer = transport.get();
  auto harness_result =
      scry::detail::HarnessTestAccess::create(config, provider(), std::move(transport));
  REQUIRE(harness_result);
  auto harness = std::move(*harness_result);
  auto first_conversation = scry::Conversation::create();
  auto second_conversation = scry::Conversation::create();
  REQUIRE(first_conversation);
  REQUIRE(second_conversation);

  auto first = harness.send(*first_conversation, "First");
  REQUIRE(first);
  transport_observer->wait_until_entered();
  auto busy = harness.send(*first_conversation, "Duplicate");
  REQUIRE_FALSE(busy);
  CHECK(busy.error().category == scry::ErrorCategory::busy);
  auto second = harness.send(*second_conversation, "Second");
  REQUIRE(second);
  CHECK(second->cancel());

  bool first_completed = false;
  bool second_cancelled = false;
  REQUIRE(first->on_completion(
      [&first_completed](const scry::Completion&) { first_completed = true; }));
  REQUIRE(second->on_cancelled(
      [&second_cancelled](const scry::Cancelled&) { second_cancelled = true; }));
  transport_observer->release();
  REQUIRE(pump_until(harness, [&] { return first_completed && second_cancelled; }));
  CHECK(transport_observer->calls() == 1);
  CHECK(first_conversation->message_count() == 2);
  CHECK(second_conversation->empty());
}

TEST_CASE("serialized turns begin in FIFO order with one active transfer") {
  auto config = test_config();
  config.limits.max_pending_turns = 3;
  auto transport = std::make_unique<ControllableTransport>();
  auto* observer = transport.get();
  auto harness_result =
      scry::detail::HarnessTestAccess::create(config, provider(), std::move(transport));
  REQUIRE(harness_result);
  auto harness = std::move(*harness_result);
  auto first_conversation = scry::Conversation::create();
  auto second_conversation = scry::Conversation::create();
  auto third_conversation = scry::Conversation::create();
  REQUIRE(first_conversation);
  REQUIRE(second_conversation);
  REQUIRE(third_conversation);

  auto first = harness.send(*first_conversation, "First FIFO request");
  REQUIRE(first);
  observer->wait_until_entered();
  auto second = harness.send(*second_conversation, "Second FIFO request");
  auto third = harness.send(*third_conversation, "Third FIFO request");
  REQUIRE(second);
  REQUIRE(third);
  CHECK(observer->calls() == 1);

  std::size_t completed = 0;
  REQUIRE(first->on_completion([&completed](const scry::Completion&) { ++completed; }));
  REQUIRE(
      second->on_completion([&completed](const scry::Completion&) { ++completed; }));
  REQUIRE(third->on_completion([&completed](const scry::Completion&) { ++completed; }));
  observer->release();
  REQUIRE(pump_until(harness, [&completed] { return completed == 3; }));

  const auto requests = observer->requests();
  REQUIRE(requests.size() == 3);
  CHECK(requests[0].find("First FIFO request") != std::string::npos);
  CHECK(requests[1].find("Second FIFO request") != std::string::npos);
  CHECK(requests[2].find("Third FIFO request") != std::string::npos);
}

TEST_CASE("pending-turn admission limit rejects before acceptance") {
  auto config = test_config();
  config.limits.max_pending_turns = 1;
  auto transport = std::make_unique<ControllableTransport>();
  auto* transport_observer = transport.get();
  auto harness_result =
      scry::detail::HarnessTestAccess::create(config, provider(), std::move(transport));
  REQUIRE(harness_result);
  auto harness = std::move(*harness_result);
  auto first_conversation = scry::Conversation::create();
  auto second_conversation = scry::Conversation::create();
  REQUIRE(first_conversation);
  REQUIRE(second_conversation);

  auto first = harness.send(*first_conversation, "First");
  REQUIRE(first);
  transport_observer->wait_until_entered();
  auto rejected = harness.send(*second_conversation, "Second");
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().category == scry::ErrorCategory::resource_limit);
  transport_observer->release();
  bool completed = false;
  REQUIRE(first->on_completion(
      [&completed](const scry::Completion&) { completed = true; }));
  REQUIRE(pump_until(harness, [&completed] { return completed; }));
}

TEST_CASE("dropping a Turn detaches without cancelling its callbacks or commit") {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  fake->enqueue(successful_exchange());
  auto harness_result = scry::detail::HarnessTestAccess::create(
      test_config(), provider(), std::move(fake));
  auto conversation = scry::Conversation::create();
  REQUIRE(harness_result);
  REQUIRE(conversation);

  bool completed = false;
  {
    auto turn = harness_result->send(*conversation, "Detached");
    REQUIRE(turn);
    REQUIRE(turn->on_completion(
        [&completed](const scry::Completion&) { completed = true; }));
  }

  REQUIRE(pump_until(*harness_result, [&completed] { return completed; }));
  CHECK(conversation->message_count() == 2);
}

TEST_CASE("two Harness instances keep provider and worker state isolated") {
  auto first_transport = std::make_unique<scry::test::FakeTransport>();
  auto second_transport = std::make_unique<scry::test::FakeTransport>();
  first_transport->enqueue(successful_exchange());
  second_transport->enqueue(successful_exchange());
  auto first = scry::detail::HarnessTestAccess::create(test_config(), provider(),
                                                       std::move(first_transport));
  auto second = scry::detail::HarnessTestAccess::create(test_config(), provider(),
                                                        std::move(second_transport));
  REQUIRE(first);
  REQUIRE(second);
  auto first_conversation = scry::Conversation::create();
  auto second_conversation = scry::Conversation::create();
  REQUIRE(first_conversation);
  REQUIRE(second_conversation);

  auto first_completion = first->send_and_wait(*first_conversation, "First");
  auto second_completion = second->send_and_wait(*second_conversation, "Second");
  REQUIRE(first_completion);
  REQUIRE(second_completion);
  CHECK(first_conversation->message_count() == 2);
  CHECK(second_conversation->message_count() == 2);
}
