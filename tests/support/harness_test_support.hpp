#pragma once

#include "core/provider.hpp"
#include "runtime/test_access.hpp"
#include "support/transport/fake_transport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <scry/scry.hpp>
#include <scry/testing/streams.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// Helpers shared by every suite that drives a real Harness over a scripted
// transport. Suites that need a deliberately different shape - a live curl
// endpoint, retries left enabled, a byte-at-a-time stream - keep their own
// variant next to the tests that depend on it.
namespace scry::test_support {

// Port 1 is never listening, so a suite that accidentally drops its scripted
// transport fails fast instead of reaching the network.
[[nodiscard]] inline scry::Config test_config() {
  auto config = scry::Config{
      .base_url = "http://127.0.0.1:1",
      .api_key = "sanitized-test-key",
      .model = "test-model",
  };
  config.retry.max_attempts = 1;
  config.retry.jitter_ratio = 0.0;
  return config;
}

[[nodiscard]] inline std::unique_ptr<scry::detail::ProviderAdapter>
provider(const scry::ProviderDialect dialect = scry::ProviderDialect::anthropic) {
  return scry::detail::make_provider_adapter(dialect);
}

[[nodiscard]] inline scry::test::ScriptedExchange
scripted_exchange(const std::string_view stream,
                  std::string request_id = "transport-request") {
  return {
      .body_chunks = {std::string{stream}},
      .result =
          scry::detail::TransportResult{
              .status_code = 200,
              .provider_request_id = std::move(request_id),
          },
  };
}

// The stream builders now live in the installed scry::testing component; the
// suites keep their unqualified spelling so scry's own tests exercise exactly
// the bodies a downstream consumer scripts.
using scry::testing::anthropic_error_body;
using scry::testing::anthropic_text_stream;
using scry::testing::anthropic_tool_stream;
using scry::testing::openai_error_body;
using scry::testing::openai_text_stream;
using scry::testing::openai_tool_stream;
using scry::testing::ToolUseBlock;

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

template <typename Predicate>
[[nodiscard]] bool pump_until(scry::Harness& harness, Predicate&& predicate) {
  constexpr std::size_t maximum_pumps = 100'000;
  for (std::size_t pump = 0; pump < maximum_pumps; ++pump) {
    static_cast<void>(harness.update());
    if (predicate()) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

// Delivers at most one callback per update so a test can observe the ordering
// the pump imposes between successive deliveries.
template <typename Predicate>
[[nodiscard]] bool pump_one_until(scry::Harness& harness, Predicate&& predicate) {
  constexpr std::size_t maximum_pumps = 100'000;
  for (std::size_t pump = 0; pump < maximum_pumps; ++pump) {
    static_cast<void>(harness.update({.max_callbacks = 1}));
    if (predicate()) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

// Wall-clock variant for suites driving a live endpoint, where progress depends
// on the network rather than on a bounded number of updates. The final update
// and check after the deadline keep a transfer that landed right on the
// deadline from being reported as a timeout.
template <typename Predicate>
[[nodiscard]] bool
pump_until_deadline(scry::Harness& harness, Predicate&& predicate,
                    const std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    static_cast<void>(harness.update());
    if (predicate()) {
      return true;
    }
    std::this_thread::yield();
  }
  static_cast<void>(harness.update());
  return predicate();
}

// A Harness over a scripted FakeTransport plus the Conversation to drive it.
// The transport pointer stays valid for the fixture's lifetime because the
// Harness owns the transport it was created with.
struct HarnessFixture {
  scry::test::FakeTransport* transport;
  scry::Harness harness;
  scry::Conversation conversation;
};

[[nodiscard]] inline HarnessFixture make_harness_fixture(
    scry::Config config, std::vector<scry::test::ScriptedExchange> exchanges,
    const scry::ProviderDialect dialect = scry::ProviderDialect::anthropic) {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  auto* observer = fake.get();
  for (auto& exchange : exchanges) {
    fake->enqueue(std::move(exchange));
  }
  return HarnessFixture{
      .transport = observer,
      .harness = unwrap(scry::detail::HarnessTestAccess::create(
          std::move(config), provider(dialect), std::move(fake))),
      .conversation = unwrap(scry::Conversation::create()),
  };
}

} // namespace scry::test_support
