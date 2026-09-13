#pragma once

#include "runtime/test_access.hpp"
#include "support/harness_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace scry::test_support {

inline const std::string two_tool_stream = anthropic_tool_stream({
    {.id = "call-a", .name = "first_tool", .arguments = R"({"ordinal":1})"},
    {.id = "call-b", .name = "second_tool", .arguments = R"({"ordinal":2})"},
});

inline const std::string final_stream =
    anthropic_text_stream("all done", "msg_final", {}, 7, 5);

[[nodiscard]] inline scry::ToolDefinition ordinal_tool_definition(std::string name) {
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

// Two tool calls whose names alone are large enough to overrun a tightened
// event-queue budget.
[[nodiscard]] inline std::string
large_tool_batch_stream(const std::string_view first, const std::string_view second) {
  return anthropic_tool_stream({
      {.id = "call-a", .name = first},
      {.id = "call-b", .name = second},
  });
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
