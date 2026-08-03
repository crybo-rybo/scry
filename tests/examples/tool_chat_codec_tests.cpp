#include "tool_codec.hpp"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <string_view>

namespace {

TEST_CASE("tool chat codec decodes typed arithmetic arguments") {
  const auto decoded = scry_tool_chat::decode_arithmetic_arguments(
      scry::Json{.text = R"({"b":-2.5,"a":4})"});

  REQUIRE(decoded);
  CHECK(decoded->a == 4.0);
  CHECK(decoded->b == -2.5);
}

TEST_CASE("tool chat codec rejects arguments outside its explicit schema") {
  constexpr std::string_view invalid_arguments[] = {
      "{}",
      R"({"a":1})",
      R"({"b":2})",
      R"({"a":null,"b":2})",
      R"({"a":"1","b":2})",
      R"({"a":1,"b":2,"extra":3})",
      R"({"nested":{"a":1},"b":2})",
      "[]",
  };

  for (const auto input : invalid_arguments) {
    const auto decoded = scry_tool_chat::decode_arithmetic_arguments(
        scry::Json{.text = std::string{input}});
    REQUIRE_FALSE(decoded);
    CHECK(decoded.error().category == scry::ErrorCategory::tool);
  }
}

TEST_CASE("tool chat codec emits JSON for finite typed results") {
  const auto encoded = scry_tool_chat::encode_arithmetic_result(
      scry_tool_chat::ArithmeticResult{.result = 42.5});

  REQUIRE(encoded);
  CHECK(encoded->text == R"({"result":42.5})");
}

TEST_CASE("tool chat codec rejects non-finite typed results") {
  const auto encoded =
      scry_tool_chat::encode_arithmetic_result(scry_tool_chat::ArithmeticResult{
          .result = std::numeric_limits<double>::infinity(),
      });

  REQUIRE_FALSE(encoded);
  CHECK(encoded.error().category == scry::ErrorCategory::tool);
}

} // namespace
