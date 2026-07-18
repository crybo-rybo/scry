#pragma once

#include "core/provider.hpp"
#include "runtime/test_access.hpp"
#include "support/transport/fake_transport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <memory>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace scry::test_support {

inline constexpr std::string_view two_tool_stream = R"(event: message_start
data: {"type":"message_start","message":{"id":"msg_tools","type":"message","role":"assistant","content":[],"model":"test-model","stop_reason":null,"usage":{"input_tokens":3,"output_tokens":0}}}

event: content_block_start
data: {"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"call-a","name":"first_tool","input":{}}}

event: content_block_delta
data: {"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{\"ordinal\":1}"}}

event: content_block_stop
data: {"type":"content_block_stop","index":0}

event: content_block_start
data: {"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"call-b","name":"second_tool","input":{}}}

event: content_block_delta
data: {"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"ordinal\":2}"}}

event: content_block_stop
data: {"type":"content_block_stop","index":1}

event: message_delta
data: {"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":2}}

event: message_stop
data: {"type":"message_stop"}

)";

inline constexpr std::string_view final_stream = R"(event: message_start
data: {"type":"message_start","message":{"id":"msg_final","type":"message","role":"assistant","content":[],"model":"test-model","stop_reason":null,"usage":{"input_tokens":7,"output_tokens":0}}}

event: content_block_start
data: {"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}}

event: content_block_delta
data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"all done"}}

event: content_block_stop
data: {"type":"content_block_stop","index":0}

event: message_delta
data: {"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":5}}

event: message_stop
data: {"type":"message_stop"}

)";

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

[[nodiscard]] inline std::unique_ptr<scry::detail::ProviderAdapter> provider() {
  auto result = scry::detail::make_provider_adapter(scry::ProviderDialect::anthropic);
  REQUIRE(result);
  return std::move(*result);
}

[[nodiscard]] inline scry::test::ScriptedExchange
scripted_exchange(const std::string_view stream, std::string request_id) {
  return {
      .body_chunks = {std::string{stream}},
      .result =
          scry::detail::TransportResult{
              .status_code = 200,
              .provider_request_id = std::move(request_id),
          },
  };
}

[[nodiscard]] inline scry::ToolDefinition tool_definition(std::string name) {
  return {
      .name = std::move(name),
      .description = "Accepts an explicit ordinal",
      .input_schema =
          {
              .text =
                  R"({"type":"object","properties":{"ordinal":{"type":"integer"}},"required":["ordinal"],"additionalProperties":false})",
          },
  };
}

[[nodiscard]] inline scry::ToolHandler static_handler(std::string result) {
  return [result = std::move(result)](scry::Json) -> scry::Result<scry::Json> {
    return scry::Json{.text = result};
  };
}

[[nodiscard]] inline std::string tool_block_events(const std::size_t index,
                                                   const std::string_view id,
                                                   const std::string_view name) {
  return "event: content_block_start\n"
         "data: {\"type\":\"content_block_start\",\"index\":" +
         std::to_string(index) + ",\"content_block\":{\"type\":\"tool_use\",\"id\":\"" +
         std::string{id} + "\",\"name\":\"" + std::string{name} +
         "\",\"input\":{}}}\n\n"
         "event: content_block_stop\n"
         "data: {\"type\":\"content_block_stop\",\"index\":" +
         std::to_string(index) + "}\n\n";
}

[[nodiscard]] inline std::string
large_tool_batch_stream(const std::string_view first, const std::string_view second) {
  return std::string{R"(event: message_start
data: {"type":"message_start","message":{"id":"msg_tools","type":"message","role":"assistant","content":[],"stop_reason":null}}

)"} + tool_block_events(0, "call-a", first) +
         tool_block_events(1, "call-b", second) + R"(event: message_delta
data: {"type":"message_delta","delta":{"stop_reason":"tool_use"}}

event: message_stop
data: {"type":"message_stop"}

)";
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

template <typename Predicate>
[[nodiscard]] bool pump_one_until(scry::Harness& harness, Predicate&& predicate) {
  constexpr std::size_t maximum_pumps = 100'000;
  for (std::size_t pump = 0; pump < maximum_pumps; ++pump) {
    static_cast<void>(harness.update({.max_callbacks = 1}));
    if (std::forward<Predicate>(predicate)()) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

inline void require_order(const std::string& text, const std::string_view first,
                          const std::string_view second) {
  const auto first_position = text.find(first);
  const auto second_position = text.find(second);
  REQUIRE(first_position != std::string::npos);
  REQUIRE(second_position != std::string::npos);
  CHECK(first_position < second_position);
}

} // namespace scry::test_support
