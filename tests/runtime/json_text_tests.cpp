// The text-level entry points in kernel/json_text.hpp are thin layers over
// Scrydantic's public JSON core, whose own tests hold it to the golden fixtures. So
// each entry point must give the core's verdict and canonical text, and report a
// rejection as a Scry Error under the caller's category and message, followed by the
// core's reason when the text is not JSON. The helpers
// Scry spells itself are checked against literal text.

#include "kernel/json_text.hpp"

#include <catch2/catch_test_macros.hpp>
#include <scry/error.hpp>
#include <scrydantic/json.hpp>
#include <string>
#include <string_view>

namespace {

constexpr auto category = scry::ErrorCategory::protocol;
constexpr std::string_view failure = "test JSON is not valid";

void check_rejection(const scry::Error& error, const std::string_view message) {
  CHECK(error.category == category);
  CHECK(error.message == message);
  CHECK_FALSE(error.retryable);
  CHECK(error.model_message.empty());
}

} // namespace

TEST_CASE("the JSON text helpers give the core's verdict under the caller's "
          "category") {
  for (const std::string_view input : {
           R"( { "b" : 1 , "a" : [ 2 , { "d" : 3 , "c" : 4 } ] } )",
           "[1.0,-0,1e2]",
           R"("text")",
           "{",
           "",
           R"({"a":1} x)",
           " \n\t[]",
       }) {
    INFO(input);
    const scrydantic::Json text{.text = std::string{input}};
    const auto view = scrydantic::JsonView::parse(text);
    const bool valid = scrydantic::is_valid_json(input);
    REQUIRE(view.has_value() == valid);
    const bool object = valid && view->kind() == scrydantic::JsonKind::object;

    const auto validated = scry::detail::validate_json(input, category, failure);
    REQUIRE(validated.has_value() == valid);
    const auto validated_object =
        scry::detail::validate_json_object(input, category, failure);
    REQUIRE(validated_object.has_value() == object);
    const auto parsed = scry::detail::parse_json_object(input, category, failure);
    REQUIRE(parsed.has_value() == object);
    const auto canonical = scry::detail::canonicalize_json(text, category, failure);
    REQUIRE(canonical.has_value() == valid);
    const auto canonical_object =
        scry::detail::canonicalize_json_object(text, category, failure);
    REQUIRE(canonical_object.has_value() == object);

    std::string message{failure};
    if (!valid) {
      message.append(": ").append(view.error().message);
    }
    if (valid) {
      CHECK(canonical->text == view->to_json().text);
    } else {
      check_rejection(validated.error(), message);
      check_rejection(canonical.error(), message);
    }
    if (object) {
      CHECK(canonical_object->text == canonical->text);
      CHECK(parsed->to_json().text == canonical->text);
    } else {
      check_rejection(parsed.error(), message);
      check_rejection(validated_object.error(), message);
      check_rejection(canonical_object.error(), message);
    }
  }
}

TEST_CASE("a JSON text rejection locates the fault and repeats none of the text") {
  const auto rejected =
      scry::detail::validate_json("{\"secret\":1,\n \"b\" 2}", category, failure);
  REQUIRE_FALSE(rejected);
  check_rejection(rejected.error(), "test JSON is not valid: JSON text is not valid: "
                                    "expected ':' at line 2, column 6");
}

TEST_CASE("json_root_is_object reads only the first byte after whitespace") {
  CHECK(scry::detail::json_root_is_object("{"));
  CHECK(scry::detail::json_root_is_object(" \t\n\r{\"a\":1}"));
  CHECK_FALSE(scry::detail::json_root_is_object(""));
  CHECK_FALSE(scry::detail::json_root_is_object(" \t\n\r"));
  CHECK_FALSE(scry::detail::json_root_is_object("[{}]"));
  CHECK_FALSE(scry::detail::json_root_is_object("\f{}"));
}

TEST_CASE("make_json_error_object quotes the message as canonical JSON") {
  CHECK(scry::detail::make_json_error_object("").text == R"({"error":""})");
  CHECK(scry::detail::make_json_error_object("bad \"quote\"\n\t\\\x01").text ==
        R"({"error":"bad \"quote\"\n\t\\\u0001"})");
}
