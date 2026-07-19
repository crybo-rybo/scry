#include "provider/wire_json.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace scry;
using namespace scry::detail;

[[nodiscard]] WireValue wire(const std::string_view text) {
  auto value = parse_wire_json(text, ErrorCategory::protocol, "invalid test JSON");
  REQUIRE(value);
  return std::move(*value);
}

} // namespace

TEST_CASE("wire JSON accessors distinguish absence, null, type, and value") {
  const auto scalar = wire("7");
  CHECK(wire_field(scalar, "value") == nullptr);
  const auto object =
      wire(R"({"string":"value","array":[],"null":null,"uint":7,"wrong":false})");
  CHECK(wire_field(object, "missing") == nullptr);
  CHECK(std::string{*required_wire_string(object, "string")} == "value");
  for (const auto name : {"missing", "wrong"}) {
    const auto result = required_wire_string(object, name);
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::protocol);
  }
  REQUIRE(required_wire_array(object, "array"));
  for (const auto name : {"missing", "wrong"}) {
    REQUIRE_FALSE(required_wire_array(object, name));
  }
  CHECK_FALSE(*optional_wire_string(object, "missing"));
  CHECK_FALSE(*optional_wire_string(object, "null"));
  REQUIRE_FALSE(optional_wire_string(object, "wrong"));
  CHECK(std::string{**optional_wire_string(object, "string")} == "value");
  CHECK_FALSE(*optional_wire_uint(object, "missing"));
  CHECK_FALSE(*optional_wire_uint(object, "null"));
  REQUIRE_FALSE(optional_wire_uint(object, "wrong"));
  CHECK(**optional_wire_uint(object, "uint") == 7);
}
