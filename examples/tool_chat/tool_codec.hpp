#pragma once

#include <scry/error.hpp>
#include <scry/json.hpp>
#include <string_view>

namespace scry_tool_chat {

struct ArithmeticArguments {
  double a{};
  double b{};
};

struct ArithmeticResult {
  double result{};
};

inline constexpr std::string_view arithmetic_input_schema =
    R"({"type":"object","properties":{"a":{"type":"number","description":"First operand"},"b":{"type":"number","description":"Second operand"}},"required":["a","b"],"additionalProperties":false})";

[[nodiscard]] scry::Result<ArithmeticArguments>
decode_arithmetic_arguments(const scry::Json& input);

[[nodiscard]] scry::Result<scry::Json>
encode_arithmetic_result(const ArithmeticResult& result);

} // namespace scry_tool_chat
