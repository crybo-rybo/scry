#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <string>
#include <string_view>

namespace {

using scry::JsonKind;
using scry::JsonView;

} // namespace

TEST_CASE("the public JSON view exposes canonical scalar kinds") {
  auto document = JsonView::parse(scry::Json{
      .text = R"({"bool":true,"float":1.5,"signed":-4,"text":"hi","unsigned":5})"});

  REQUIRE(document);
  REQUIRE(document->kind() == JsonKind::object);
  REQUIRE(document->size() == 5);
  CHECK(document->key_at(0) == "bool");
  CHECK(document->key_at(4) == "unsigned");
  CHECK_FALSE(document->key_at(5));

  const auto boolean = document->find("bool");
  REQUIRE(boolean);
  CHECK(boolean->kind() == JsonKind::boolean);
  CHECK(boolean->boolean() == true);
  CHECK_FALSE(boolean->string());

  const auto floating = document->find("float");
  REQUIRE(floating);
  CHECK(floating->kind() == JsonKind::number);
  CHECK(floating->number() == 1.5);

  const auto signed_integer = document->find("signed");
  REQUIRE(signed_integer);
  CHECK(signed_integer->kind() == JsonKind::signed_integer);
  CHECK(signed_integer->signed_integer() == -4);

  const auto unsigned_integer = document->find("unsigned");
  REQUIRE(unsigned_integer);
  CHECK(unsigned_integer->kind() == JsonKind::unsigned_integer);
  CHECK(unsigned_integer->unsigned_integer() == std::uint64_t{5});

  const auto text = document->find("text");
  REQUIRE(text);
  CHECK(text->kind() == JsonKind::string);
  CHECK(text->string() == "hi");
  CHECK_FALSE(text->boolean());
  CHECK_FALSE(text->signed_integer());
  CHECK_FALSE(text->unsigned_integer());
  CHECK_FALSE(text->number());
  CHECK(text->size() == 0);
  CHECK_FALSE(text->at(0));
  CHECK_FALSE(text->key_at(0));
}

TEST_CASE("the public JSON view reads every whole number as a double") {
  const auto document =
      JsonView::parse(scry::Json{.text = R"({"signed":-4,"unsigned":5})"});

  REQUIRE(document);
  const auto signed_integer = document->find("signed");
  REQUIRE(signed_integer);
  CHECK(signed_integer->number() == -4.0);

  const auto unsigned_integer = document->find("unsigned");
  REQUIRE(unsigned_integer);
  CHECK(unsigned_integer->number() == 5.0);
}

TEST_CASE("the public JSON view visits object keys in lexicographic order") {
  const auto document = JsonView::parse(scry::Json{.text = R"({"b":1,"a":2})"});

  REQUIRE(document);
  REQUIRE(document->size() == 2);
  CHECK(document->key_at(0) == "a");
  CHECK(document->key_at(1) == "b");
  CHECK_FALSE(document->key_at(2));
}

TEST_CASE("the public JSON view finds members of a nested object") {
  const auto document = JsonView::parse(
      scry::Json{.text = R"({"outer":{"inner":{"leaf":"found"}},"other":1})"});

  REQUIRE(document);
  const auto outer = document->find("outer");
  REQUIRE(outer);
  const auto inner = outer->find("inner");
  REQUIRE(inner);
  const auto leaf = inner->find("leaf");
  REQUIRE(leaf);
  CHECK(leaf->string() == "found");
  CHECK_FALSE(inner->find("missing"));
  CHECK_FALSE(leaf->find("leaf"));
}

TEST_CASE("the public JSON view rejects an out-of-range array index") {
  const auto document = JsonView::parse(scry::Json{.text = R"([10,20])"});

  REQUIRE(document);
  REQUIRE(document->kind() == JsonKind::array);
  REQUIRE(document->size() == 2);
  const auto first = document->at(0);
  REQUIRE(first);
  CHECK(first->unsigned_integer() == std::uint64_t{10});
  CHECK_FALSE(document->at(2));
  CHECK_FALSE(document->at(std::size_t{1} << 40U));
}

TEST_CASE("the public JSON view retains nested document lifetime") {
  auto document =
      JsonView::parse(scry::Json{.text = R"({"items":[null,{"name":"kept"}]})"});

  REQUIRE(document);
  auto items = document->find("items");
  REQUIRE(items);
  REQUIRE(items->kind() == JsonKind::array);
  REQUIRE(items->size() == 2);

  document = std::unexpected(scry::Error{});

  const auto null_value = items->at(0);
  REQUIRE(null_value);
  CHECK(null_value->kind() == JsonKind::null);

  const auto object = items->at(1);
  REQUIRE(object);
  const auto name = object->find("name");
  REQUIRE(name);
  CHECK(name->string() == "kept");
  CHECK_FALSE(items->at(2));
  CHECK_FALSE(items->key_at(0));
  CHECK_FALSE(items->find("name"));
}

TEST_CASE("an empty public JSON view is safely inspectable") {
  const JsonView view{};

  CHECK(view.kind() == JsonKind::null);
  CHECK_FALSE(view.boolean());
  CHECK_FALSE(view.signed_integer());
  CHECK_FALSE(view.unsigned_integer());
  CHECK_FALSE(view.number());
  CHECK_FALSE(view.string());
  CHECK(view.size() == 0);
  CHECK_FALSE(view.at(0));
  CHECK_FALSE(view.key_at(0));
  CHECK_FALSE(view.find("anything"));
}

TEST_CASE("the public JSON view rejects malformed input") {
  const auto document = JsonView::parse(scry::Json{.text = "{"});

  REQUIRE_FALSE(document);
  CHECK(document.error().category == scry::ErrorCategory::invalid_argument);
  CHECK(document.error().message == "JSON text is not valid");
}

TEST_CASE("escape_json_string quotes and escapes control characters") {
  CHECK(scry::escape_json_string(std::string_view{"quote\" slash\\\b\f\n\r\t\x01"}) ==
        R"("quote\" slash\\\b\f\n\r\t\u0001")");
  CHECK(scry::escape_json_string("") == R"("")");
}

TEST_CASE("escape_json_string passes non-ASCII UTF-8 bytes through unescaped") {
  const std::string value = "caf\xc3\xa9 \xe2\x9c\x93";

  CHECK(scry::escape_json_string(value) == "\"" + value + "\"");
}

TEST_CASE("the public JSON view rejects truncated and trailing-garbage documents") {
  // Glaze treats the end of a non-NUL-terminated buffer as an implicit
  // terminator, so each of these parsed before the codec validated the whole
  // document. A body that stops mid-value is a broken response, not a short one.
  const std::string_view rejected[] = {
      R"({"error":{"type":"not_found_error")", // truncated object
      R"([1,2)",                               // truncated array
      R"({"a")",                               // key with no value
      R"({"a":1} x)",                          // trailing garbage
      R"({"a":1}{"b":2})",                     // a second document
      R"(123abc)",                             // a number followed by junk
  };

  for (const auto text : rejected) {
    const auto document = JsonView::parse(scry::Json{.text = std::string{text}});
    REQUIRE_FALSE(document);
    CHECK(document.error().category == scry::ErrorCategory::invalid_argument);
    CHECK(document.error().message == "JSON text is not valid");
  }

  // The complete forms of the same documents still parse.
  CHECK(JsonView::parse(scry::Json{.text = R"({"error":{"type":"not_found_error"}})"}));
  CHECK(JsonView::parse(scry::Json{.text = R"([1,2])"}));
  CHECK(JsonView::parse(scry::Json{.text = R"({"a":1} )"}));
  CHECK(JsonView::parse(scry::Json{.text = R"(123)"}));
}
