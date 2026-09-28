#include "backend/http_backend.hpp"
#include "core/backend.hpp"
#include "core/provider.hpp"
#include "runtime/test_access.hpp"
#include "support/harness_test_support.hpp"
#include "support/transport/fake_transport.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <scry/scry.hpp>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace scry::test_support;

namespace {

using scry::detail::HttpStreamBackend;
using scry::detail::ModelResponse;
using scry::detail::ProviderCompleted;
using scry::detail::ProviderEvent;
using scry::detail::ProviderEventSink;
using scry::detail::ProviderSemanticOutput;
using scry::detail::ProviderTextDelta;

[[nodiscard]] scry::detail::ModelRequest user_request(const scry::Config& config) {
  return scry::detail::ModelRequest{
      .system_prompt = {},
      .history = {},
      .messages = {scry::Message{
          .role = scry::Role::user,
          .content = {scry::TextBlock{.text = "hello"}},
      }},
      .tools = {},
      .sampling = config.sampling,
  };
}

// Runs one attempt over a single scripted exchange and keeps every event the
// backend streamed.
struct AttemptRun {
  scry::Result<ModelResponse> response{};
  std::vector<ProviderEvent> events{};
};

[[nodiscard]] AttemptRun
run_attempt(const scry::Config& config, scry::test::ScriptedExchange exchange,
            const scry::ProviderDialect dialect = scry::ProviderDialect::anthropic) {
  auto transport = std::make_unique<scry::test::FakeTransport>();
  transport->enqueue(std::move(exchange));
  HttpStreamBackend backend{provider(dialect), std::move(transport)};
  AttemptRun run{};
  ProviderEventSink sink{[&run](ProviderEvent event) -> scry::Status {
    run.events.push_back(std::move(event));
    return {};
  }};
  const std::atomic<bool> cancelled{false};
  run.response =
      backend.perform(config, user_request(config), std::stop_token{}, cancelled, sink);
  return run;
}

template <typename Event>
[[nodiscard]] std::size_t count_events(const std::vector<ProviderEvent>& events) {
  std::size_t count = 0;
  for (const auto& event : events) {
    if (std::holds_alternative<Event>(event)) {
      ++count;
    }
  }
  return count;
}

} // namespace

TEST_CASE("the HTTP backend reports semantic output once, ahead of streamed text") {
  auto run = run_attempt(test_config(),
                         scripted_exchange(anthropic_text_stream("streamed text")));

  REQUIRE(run.response);
  REQUIRE(run.events.size() == 2);
  CHECK(std::holds_alternative<ProviderSemanticOutput>(run.events[0]));
  CHECK(std::get<ProviderTextDelta>(run.events[1]).text == "streamed text");
  CHECK(count_events<ProviderCompleted>(run.events) == 0);
  CHECK(run.response->finish_reason == scry::FinishReason::completed);
}

TEST_CASE("the HTTP backend reports semantic output for a tool-call-only stream") {
  // OpenAI tool-call fragments produce no provider event of their own, so the
  // semantic-output marker is the only thing the worker hears before the
  // completion that ends retry eligibility.
  auto config = test_config();
  config.dialect = scry::ProviderDialect::openai_compatible;
  auto run = run_attempt(
      config,
      scripted_exchange(openai_tool_stream(
          {{.id = "call_1", .name = "lookup", .arguments = R"({"q":"x"})"}})),
      scry::ProviderDialect::openai_compatible);

  REQUIRE(run.response);
  REQUIRE(run.events.size() == 1);
  CHECK(std::holds_alternative<ProviderSemanticOutput>(run.events[0]));
  CHECK(run.response->finish_reason == scry::FinishReason::tool_use);
  REQUIRE(run.response->content.size() == 1);
  CHECK(std::get<scry::ToolCallBlock>(run.response->content[0]).name == "lookup");
}

TEST_CASE("the HTTP backend prefers the stream's request identifier over the "
          "transport's") {
  auto from_transport =
      run_attempt(test_config(),
                  scripted_exchange(anthropic_text_stream("a"), "transport-request"));
  auto from_stream = run_attempt(
      test_config(),
      scripted_exchange(anthropic_text_stream("b", "msg_b", "stream-request"),
                        "transport-request"));

  REQUIRE(from_transport.response);
  REQUIRE(from_stream.response);
  CHECK(from_transport.response->provider_request_id == "transport-request");
  CHECK(from_stream.response->provider_request_id == "stream-request");
}

TEST_CASE("a failed sink status aborts the HTTP attempt and is returned") {
  auto transport = std::make_unique<scry::test::FakeTransport>();
  transport->enqueue(scripted_exchange(anthropic_text_stream("never delivered")));
  HttpStreamBackend backend{provider(), std::move(transport)};
  std::size_t delivered = 0;
  ProviderEventSink sink{[&delivered](ProviderEvent) -> scry::Status {
    ++delivered;
    return std::unexpected(scry::Error{
        .category = scry::ErrorCategory::resource_limit,
        .message = "sink refused the event",
    });
  }};
  const auto config = test_config();
  const std::atomic<bool> cancelled{false};

  const auto response =
      backend.perform(config, user_request(config), std::stop_token{}, cancelled, sink);

  REQUIRE_FALSE(response);
  CHECK(response.error().category == scry::ErrorCategory::resource_limit);
  CHECK(response.error().message == "sink refused the event");
  CHECK(delivered == 1);
}

TEST_CASE("an HTTP stream that ends without a completion fails with protocol") {
  auto run = run_attempt(test_config(), scripted_exchange(""));

  REQUIRE_FALSE(run.response);
  CHECK(run.response.error().category == scry::ErrorCategory::protocol);
  CHECK(run.response.error().message ==
        "provider stream ended without a completion event");
  CHECK(run.events.empty());
}

TEST_CASE("a tool-call fragment ends retry eligibility on the HTTP path") {
  // Only the first frame of an OpenAI tool stream: it opens a tool call and
  // yields no text, then the transfer fails with a retryable error. The
  // fragment alone must count as semantic output, so the turn is not retried.
  const auto stream = openai_tool_stream({{.id = "call_1", .name = "lookup"}});
  const auto first_frame = stream.substr(0, stream.find("\n\n") + 2);
  REQUIRE(first_frame.find("tool_calls") != std::string::npos);
  auto config = test_config();
  config.dialect = scry::ProviderDialect::openai_compatible;
  config.retry.max_attempts = 3;
  config.retry.initial_backoff = std::chrono::milliseconds{1};
  config.retry.max_backoff = std::chrono::milliseconds{1};
  auto transport = std::make_unique<scry::test::FakeTransport>();
  auto* observer = transport.get();
  transport->enqueue(scry::test::ScriptedExchange{
      .body_chunks = {first_frame},
      .result = std::unexpected(scry::Error{
          .category = scry::ErrorCategory::network,
          .retryable = true,
          .message = "scripted transfer failure",
      }),
  });
  transport->enqueue(scripted_exchange(openai_text_stream("must not be requested")));
  auto harness = unwrap(scry::detail::HarnessTestAccess::create(
      config, provider(scry::ProviderDialect::openai_compatible),
      std::move(transport)));
  auto conversation = unwrap(scry::Conversation::create());

  const auto completion = harness.send_and_wait(conversation, "fragment then fail");

  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::network);
  CHECK(completion.error().message == "scripted transfer failure");
  CHECK(observer->calls() == 1);
}
