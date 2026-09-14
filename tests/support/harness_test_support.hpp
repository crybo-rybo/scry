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

// One tool call of an OpenAI-compatible stream. `arguments` is the tool input as
// plain JSON; the builder escapes it into the delta's `arguments` string.
struct OpenAiToolCall {
  std::string_view id{};
  std::string_view name{};
  std::string_view arguments{};
};

// The OpenAI-compatible counterpart of anthropic_tool_stream: a role chunk, one
// delta per call, the tool_calls finish chunk, usage, then the sentinel. Each
// call arrives whole because fragment reassembly is covered by the suites that
// script their own bytes.
[[nodiscard]] inline std::string
openai_tool_call_stream(const std::initializer_list<OpenAiToolCall> calls,
                        const std::string_view completion_id = "chatcmpl-tools",
                        const std::uint32_t prompt_tokens = 4,
                        const std::uint32_t completion_tokens = 3) {
  const auto prefix = R"(data: {"id":")" + std::string{completion_id} +
                      R"(","object":"chat.completion.chunk","choices":)";
  auto stream = prefix;
  stream += R"([{"index":0,"delta":{"role":"assistant"},"finish_reason":null}]})";
  stream += "\n\n";
  auto index = std::size_t{0};
  for (const auto& call : calls) {
    stream += prefix;
    stream += R"([{"index":0,"delta":{"tool_calls":[{"index":)";
    stream += std::to_string(index);
    stream += R"(,"id":")";
    stream += call.id;
    stream += R"(","type":"function","function":{"name":")";
    stream += call.name;
    stream += R"(","arguments":")";
    stream += quoted_json(call.arguments);
    stream += R"("}}]},"finish_reason":null}]})";
    stream += "\n\n";
    ++index;
  }
  stream += prefix;
  stream += R"([{"index":0,"delta":{},"finish_reason":"tool_calls"}]})";
  stream += "\n\n";
  stream += prefix;
  stream += R"([],"usage":{"prompt_tokens":)";
  stream += std::to_string(prompt_tokens);
  stream += R"(,"completion_tokens":)";
  stream += std::to_string(completion_tokens);
  stream += R"(,"total_tokens":)";
  stream += std::to_string(prompt_tokens + completion_tokens);
  stream += R"(}})";
  stream += "\n\ndata: [DONE]\n\n";
  return stream;
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
