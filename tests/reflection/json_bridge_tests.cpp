#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <scry/detail/reflection_json.hpp>
#include <scry/json.hpp>
#include <string>

namespace {

using scry::reflection::detail::JsonKind;

} // namespace

TEST_CASE("reflection JSON bridge exposes canonical scalar kinds") {
  auto document = scry::reflection::detail::parse_json(scry::Json{
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

TEST_CASE("reflection JSON bridge retains nested document lifetime") {
  auto document = scry::reflection::detail::parse_json(
      scry::Json{.text = R"({"items":[null,{"name":"kept"}]})"});

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

TEST_CASE("an empty reflection JSON view is safely inspectable") {
  const scry::reflection::detail::JsonView view{};

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

TEST_CASE("reflection JSON bridge rejects malformed input") {
  const auto document = scry::reflection::detail::parse_json(scry::Json{.text = "{"});

  REQUIRE_FALSE(document);
  CHECK(document.error().category == scry::ErrorCategory::tool);
  CHECK(document.error().message == "reflected tool arguments are not valid JSON");
}

TEST_CASE("reflection JSON bridge writes escaped JSON strings") {
  std::string output{};
  scry::reflection::detail::append_json_string(
      output, std::string_view{"quote\" slash\\\b\f\n\r\t\x01"});

  CHECK(output == R"("quote\" slash\\\b\f\n\r\t\u0001")");
}
