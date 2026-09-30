#include "kernel/json/codec.hpp"
#include "reflection/codec.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <scry/reflection.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Payload {
  std::string text{};
};

// Borrows everything it writes, as the request wire structs do.
struct BorrowedFrame {
  const scry::Json& body;
  const std::vector<Payload>& items;
  std::string_view label;
};

struct OwnedFrame {
  std::vector<Payload> items{};
  std::uint64_t count{};
};

} // namespace

static_assert(scry::reflection::Encodable<BorrowedFrame>);
static_assert(!scry::reflection::Decodable<BorrowedFrame>);

TEST_CASE("an encode-only aggregate may borrow members through references") {
  const scry::Json body{.text = R"({"z":1,"a":2})"};
  const std::vector<Payload> items{{.text = "one"}, {.text = "two"}};
  const auto encoded = scry::detail::encode_text(
      BorrowedFrame{.body = body, .items = items, .label = "l"});
  REQUIRE(encoded);
  // Keys are sorted; the borrowed Json is spliced byte for byte.
  CHECK(
      *encoded ==
      R"({"body":{"z":1,"a":2},"items":[{"text":"one"},{"text":"two"}],"label":"l"})");
}

TEST_CASE("the codec escapes control characters as the canonical writer does") {
  const std::string text{"\x01\x1b[0m\x1f\x7f"};
  const auto encoded = scry::detail::encode_text(Payload{.text = text});
  REQUIRE(encoded);
  CHECK(*encoded == "{\"text\":\"\\u0001\\u001B[0m\\u001F\x7f\"}");
  const auto canonical = scry::detail::canonicalize_json(
      scry::Json{.text = *encoded}, scry::ErrorCategory::invalid_argument, "invalid");
  REQUIRE(canonical);
  CHECK(canonical->text == *encoded);
}

TEST_CASE("the internal codec entry points report a path and a reason") {
  const auto view =
      scry::JsonView::parse(scry::Json{.text = R"({"items":[{"text":7}]})"});
  REQUIRE(view);
  const auto decoded = scry::detail::decode_value<OwnedFrame>(*view);
  REQUIRE_FALSE(decoded);
  CHECK(scry::detail::describe(decoded.error()) == "$.items[0].text must be a string");

  const auto invalid = scry::detail::encode_text(
      BorrowedFrame{.body = scry::Json{.text = "{"}, .items = {}, .label = ""});
  REQUIRE_FALSE(invalid);
  CHECK(scry::detail::describe(invalid.error()) == "$.body is not valid JSON");
}
