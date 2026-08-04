#pragma once

#include "runtime/test_access.hpp"
#include "support/harness_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <utility>

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

inline void require_order(const std::string& text, const std::string_view first,
                          const std::string_view second) {
  const auto first_position = text.find(first);
  const auto second_position = text.find(second);
  REQUIRE(first_position != std::string::npos);
  REQUIRE(second_position != std::string::npos);
  CHECK(first_position < second_position);
}

} // namespace scry::test_support
