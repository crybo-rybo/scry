#pragma once

#include "core/provider.hpp"
#include "runtime/test_access.hpp"
#include "support/transport/fake_transport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <scry/scry.hpp>
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

// The canonical five-event Anthropic text completion, built once instead of
// retyped in every suite that needs a plain successful turn. Streams whose exact
// bytes or malformed shape are the thing under test stay next to their tests.
[[nodiscard]] inline std::string anthropic_text_stream(
    const std::string_view text, const std::string_view message_id = "msg_test",
    const std::string_view request_id = {}, const std::uint32_t input_tokens = 2,
    const std::uint32_t output_tokens = 2) {
  auto correlation = std::string{};
  if (!request_id.empty()) {
    correlation = R"(,"request_id":")" + std::string{request_id} + R"(")";
  }
  auto stream = std::string{"event: message_start\ndata: "};
  stream += R"({"type":"message_start","message":{"id":")";
  stream += message_id;
  stream += R"(")";
  stream += correlation;
  stream += R"(,"type":"message","role":"assistant","content":[],)";
  stream += R"("model":"test-model","stop_reason":null,"usage":{"input_tokens":)";
  stream += std::to_string(input_tokens);
  stream += R"(,"output_tokens":0}}})";
  stream += "\n\nevent: content_block_start\ndata: ";
  stream += R"({"type":"content_block_start","index":0,)";
  stream += R"("content_block":{"type":"text","text":""}})";
  stream += "\n\nevent: content_block_delta\ndata: ";
  stream += R"({"type":"content_block_delta","index":0,)";
  stream += R"("delta":{"type":"text_delta","text":")";
  stream += text;
  stream += R"("}})";
  stream += "\n\nevent: content_block_stop\ndata: ";
  stream += R"({"type":"content_block_stop","index":0})";
  stream += "\n\nevent: message_delta\ndata: ";
  stream += R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},)";
  stream += R"("usage":{"output_tokens":)";
  stream += std::to_string(output_tokens);
  stream += R"(}})";
  stream += "\n\nevent: message_stop\ndata: ";
  stream += R"({"type":"message_stop"})";
  stream += "\n\n";
  return stream;
}

// One tool_use block of an Anthropic stream. `arguments` is the tool input as
// plain JSON; the builder escapes it into the partial_json delta, and an empty
// value emits no delta at all.
struct ToolUseBlock {
  std::string_view id{};
  std::string_view name{};
  std::string_view arguments{};
};

[[nodiscard]] inline std::string quoted_json(const std::string_view text) {
  auto escaped = std::string{};
  for (const auto character : text) {
    if (character == '"' || character == '\\') {
      escaped.push_back('\\');
    }
    escaped.push_back(character);
  }
  return escaped;
}

[[nodiscard]] inline std::string tool_block_events(const std::size_t index,
                                                   const ToolUseBlock& block) {
  const auto position = std::to_string(index);
  auto events = std::string{"event: content_block_start\ndata: "};
  events += R"({"type":"content_block_start","index":)" + position;
  events += R"(,"content_block":{"type":"tool_use","id":")";
  events += block.id;
  events += R"(","name":")";
  events += block.name;
  events += R"(","input":{}}})";
  events += "\n\n";
  if (!block.arguments.empty()) {
    events += "event: content_block_delta\ndata: ";
    events += R"({"type":"content_block_delta","index":)" + position;
    events += R"(,"delta":{"type":"input_json_delta","partial_json":")";
    events += quoted_json(block.arguments);
    events += R"("}})";
    events += "\n\n";
  }
  events += "event: content_block_stop\ndata: ";
  events += R"({"type":"content_block_stop","index":)" + position + "}";
  events += "\n\n";
  return events;
}

// An Anthropic stream whose content is one or more tool_use blocks. The stop
// reason is a parameter because a stream that announces tool calls and then
// ends the turn is itself a case under test. Reported output_tokens is the
// block count.
[[nodiscard]] inline std::string
anthropic_tool_stream(const std::initializer_list<ToolUseBlock> blocks,
                      const std::string_view message_id = "msg_tools",
                      const std::string_view stop_reason = "tool_use",
                      const std::uint32_t input_tokens = 3) {
  auto stream = std::string{"event: message_start\ndata: "};
  stream += R"({"type":"message_start","message":{"id":")";
  stream += message_id;
  stream += R"(","type":"message","role":"assistant","content":[],)";
  stream += R"("model":"test-model","stop_reason":null,"usage":{"input_tokens":)";
  stream += std::to_string(input_tokens);
  stream += R"(,"output_tokens":0}}})";
  stream += "\n\n";
  auto index = std::size_t{0};
  for (const auto& block : blocks) {
    stream += tool_block_events(index, block);
    ++index;
  }
  stream += "event: message_delta\ndata: ";
  stream += R"({"type":"message_delta","delta":{"stop_reason":")";
  stream += stop_reason;
  stream += R"("},"usage":{"output_tokens":)";
  stream += std::to_string(blocks.size());
  stream += R"(}})";
  stream += "\n\nevent: message_stop\ndata: ";
  stream += R"({"type":"message_stop"})";
  stream += "\n\n";
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
