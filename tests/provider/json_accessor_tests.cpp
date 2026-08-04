#include "core/json_codec.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace scry;
using namespace scry::detail;

[[nodiscard]] JsonValue json_value(const std::string_view text) {
  auto value = parse_json(text, ErrorCategory::protocol, "invalid test JSON");
  REQUIRE(value);
  return std::move(*value);
}

} // namespace

TEST_CASE("JSON field accessors distinguish absence, null, type, and value") {
  const auto scalar = json_value("7");
  CHECK(json_field(scalar, "value") == nullptr);
  const auto object =
      json_value(R"({"string":"value","array":[],"null":null,"uint":7,"wrong":false})");
  CHECK(json_field(object, "missing") == nullptr);
  CHECK(std::string{*required_json_string(object, "string")} == "value");
  for (const auto name : {"missing", "wrong"}) {
    const auto result = required_json_string(object, name);
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::protocol);
  }
  REQUIRE(required_json_array(object, "array"));
  for (const auto name : {"missing", "wrong"}) {
    REQUIRE_FALSE(required_json_array(object, name));
  }
  const auto nested = json_value(R"({"nested":{}})");
  REQUIRE(required_json_object(nested, "nested"));
  for (const auto name : {"missing", "wrong"}) {
    const auto result = required_json_object(object, name);
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::protocol);
  }
  CHECK_FALSE(*optional_json_string(object, "missing"));
  CHECK_FALSE(*optional_json_string(object, "null"));
  REQUIRE_FALSE(optional_json_string(object, "wrong"));
  CHECK(std::string{**optional_json_string(object, "string")} == "value");
  CHECK_FALSE(*optional_json_uint(object, "missing"));
  CHECK_FALSE(*optional_json_uint(object, "null"));
  REQUIRE_FALSE(optional_json_uint(object, "wrong"));
  CHECK(**optional_json_uint(object, "uint") == 7);
}
