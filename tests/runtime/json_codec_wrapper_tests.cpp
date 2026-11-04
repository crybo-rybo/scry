// The text-level entry points in kernel/json/codec.hpp are thin layers over
// Scrydantic's JSON core, whose own tests hold it to the golden fixtures. So each
// entry point must give the core's verdict and canonical text, and report a
// rejection as a Scry Error under the caller's category and message.

#include "kernel/json/codec.hpp"

#include <catch2/catch_test_macros.hpp>
#include <scry/error.hpp>
#include <scrydantic/detail/json_text.hpp>
#include <scrydantic/json.hpp>
#include <string>
#include <string_view>

namespace {

constexpr auto category = scry::ErrorCategory::protocol;
constexpr std::string_view failure = "test JSON is not valid";

void check_rejection(const scry::Error& error) {
  CHECK(error.category == category);
  CHECK(error.message == failure);
  CHECK_FALSE(error.retryable);
  CHECK(error.model_message.empty());
}

} // namespace

TEST_CASE("the JSON codec wrappers give the core's verdict under the caller's "
          "category") {
  for (const std::string_view input : {
           R"( { "b" : 1 , "a" : [ 2 , { "d" : 3 , "c" : 4 } ] } )",
           "[1.0,-0,1e2]",
           R"("text")",
           "{",
           "",
           R"({"a":1} x)",
       }) {
    INFO(input);
    const scrydantic::Json text{.text = std::string{input}};
    const bool valid = scrydantic::detail::validate_json(input);
    const bool object = scrydantic::detail::validate_json_object(input);

    const auto validated = scry::detail::validate_json(input, category, failure);
    REQUIRE(validated.has_value() == valid);
    const auto validated_object =
        scry::detail::validate_json_object(input, category, failure);
    REQUIRE(validated_object.has_value() == object);
    const auto canonical = scry::detail::canonicalize_json(text, category, failure);
    REQUIRE(canonical.has_value() == valid);
    const auto canonical_object =
        scry::detail::canonicalize_json_object(text, category, failure);
    REQUIRE(canonical_object.has_value() == object);

    if (valid) {
      CHECK(canonical->text == scrydantic::detail::canonicalize_json(text)->text);
    } else {
      check_rejection(validated.error());
      check_rejection(canonical.error());
    }
    if (object) {
      CHECK(canonical_object->text == canonical->text);
    } else {
      check_rejection(validated_object.error());
      check_rejection(canonical_object.error());
    }
  }
}
