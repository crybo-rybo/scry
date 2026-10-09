#pragma once

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <scry/scry.hpp>
#include <scry/testing/scripted_server.hpp>
#include <scry/testing/streams.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// Helpers for suites that drive a Harness through the public API. A scripted
// turn goes to a scry::testing::ScriptedServer on loopback, so each test runs
// the shipping provider, transport, worker, and pump.
namespace scry::test_support {

// Port 1 is never listening, so a Harness that no test points at a server
// fails fast instead of reaching the network. Millisecond backoffs without
// jitter keep a scripted retry fast, and a short shutdown bound lets a
// cancelled transfer end promptly.
[[nodiscard]] inline scry::Config test_config() {
  auto config = scry::Config{
      .base_url = "http://127.0.0.1:1",
      .api_key = "sanitized-test-key",
      .model = "test-model",
  };
  config.retry.max_attempts = 1;
  config.retry.initial_backoff = std::chrono::milliseconds{1};
  config.retry.max_backoff = std::chrono::milliseconds{5};
  config.retry.jitter_ratio = 0.0;
  config.timeouts.shutdown = std::chrono::milliseconds{25};
  return config;
}

// The stream builders are the installed scry::testing ones, so scry's own tests
// script exactly the bodies a downstream consumer does.
using scry::testing::anthropic_error_body;
using scry::testing::anthropic_text_stream;
using scry::testing::anthropic_tool_stream;
using scry::testing::openai_error_body;
using scry::testing::openai_text_stream;
using scry::testing::openai_tool_stream;
using scry::testing::ScriptedResponse;
using scry::testing::ScriptedServer;
using scry::testing::ToolUseBlock;

// A 200 response with `body` in one chunk. A non-empty `request_id` goes out as
// the `request-id` header, which both dialects read as the correlation id.
[[nodiscard]] inline ScriptedResponse scripted_response(const std::string_view body,
                                                        std::string request_id = {}) {
  auto response = ScriptedResponse{.body_chunks = {std::string{body}}};
  if (!request_id.empty()) {
    response.headers.push_back({.name = "request-id", .value = std::move(request_id)});
  }
  return response;
}

[[nodiscard]] inline scry::ToolHandler static_handler(std::string result) {
  return [result = std::move(result)](scry::Json) -> scry::Result<scry::Json> {
    return scry::Json{.text = result};
  };
}

// Yields the value, failing the test when the result carries an error. Catch2's
// REQUIRE aborts the calling TEST_CASE from here just as it would inline.
template <typename Value> [[nodiscard]] Value unwrap(scry::Result<Value> result) {
  REQUIRE(result);
  return std::move(*result);
}

[[nodiscard]] inline ScriptedServer start_server() {
  return unwrap(ScriptedServer::create());
}

// Pumps until `predicate` holds. The deadline only guards against a hang, so it
// stays below the ctest TIMEOUT of 15 s. The final update and check after the
// deadline keep a transfer that landed right on it from being reported as a
// timeout.
template <typename Predicate>
[[nodiscard]] bool pump_until(scry::Harness& harness, Predicate&& predicate,
                              const scry::UpdateOptions options = {}) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
  while (std::chrono::steady_clock::now() < deadline) {
    static_cast<void>(harness.update(options));
    if (predicate()) {
      return true;
    }
    std::this_thread::yield();
  }
  static_cast<void>(harness.update(options));
  return predicate();
}

// Delivers at most one callback per update so a test can observe the ordering
// the pump imposes between successive deliveries.
template <typename Predicate>
[[nodiscard]] bool pump_one_until(scry::Harness& harness, Predicate&& predicate) {
  return pump_until(harness, std::forward<Predicate>(predicate), {.max_callbacks = 1});
}

// A scripted server, a Harness pointed at it, and a Conversation to drive it.
// Members are destroyed in reverse order, so the Harness stops its transfers
// before the server goes away.
struct HarnessFixture {
  ScriptedServer server;
  scry::Harness harness;
  scry::Conversation conversation;
};

// Starts a server that answers with `responses` in order, and a Harness whose
// `config` points at it.
[[nodiscard]] inline HarnessFixture
make_harness_fixture(scry::Config config, std::vector<ScriptedResponse> responses) {
  auto server = start_server();
  for (auto& response : responses) {
    server.enqueue(std::move(response));
  }
  config.base_url = server.url();
  auto harness = unwrap(scry::Harness::create(std::move(config)));
  return HarnessFixture{
      .server = std::move(server),
      .harness = std::move(harness),
      .conversation = unwrap(scry::Conversation::create()),
  };
}

} // namespace scry::test_support
