// What the JSON core accepts, seen from outside it: the text-level entry points
// in <scrydantic/detail/json_text.hpp> and the public JsonView are thin layers
// over the core's document (src/json/document.hpp), which json_fixture_tests.cpp
// holds to the golden fixtures. So every entry point must give the document's
// verdict and write the document's canonical text, checked here on a table of
// adversarial documents named by shape and a handful of representative ones.

#include "json/document.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <scrydantic/detail/json_text.hpp>
#include <scrydantic/error.hpp>
#include <scrydantic/json.hpp>
#include <string>
#include <string_view>

namespace {

using scrydantic::JsonView;
namespace json = scrydantic::detail::json;
namespace text_codec = scrydantic::detail;

// Every entry point's verdict on `input`, each checked against the document's.
[[nodiscard]] bool codec_accepts(const std::string_view input) {
  const auto document = json::parse(input);
  const scrydantic::Json text{.text = std::string{input}};
  const auto canonical = text_codec::canonicalize_json(text);
  const auto viewed = JsonView::parse(text);
  REQUIRE(text_codec::validate_json(input) == document.has_value());
  REQUIRE(canonical.has_value() == document.has_value());
  REQUIRE(viewed.has_value() == document.has_value());
  // The object variants accept exactly the documents whose root is an object.
  const bool object = document && document->kind() == scrydantic::JsonKind::object;
  CHECK(text_codec::validate_json_object(input) == object);
  CHECK(text_codec::canonicalize_json_object(text).has_value() == object);
  if (!document) {
    // The whole error: the view's failure carries nothing for a model.
    CHECK(viewed.error().message == "JSON text is not valid");
    CHECK(viewed.error().model_message.empty());
    return false;
  }
  const auto written = json::write(*document);
  CHECK(canonical->text == written);
  CHECK(viewed->to_json().text == written);
  return true;
}

struct Adversarial {
  std::string_view name{};
  std::string_view text{};
  bool accepted{};
};

} // namespace

TEST_CASE("the JSON codec rejects every shape of incomplete document") {
  static constexpr Adversarial cases[] = {
      // Truncation: the buffer stops mid-value.
      {"truncated object", R"({"a":1)", false},
      {"truncated nested object", R"({"error":{"type":"not_found_error")", false},
      {"truncated array", R"([1,2)", false},
      {"key with no value", R"({"a")", false},
      {"key with colon and no value", R"({"a":)", false},
      {"truncated string", R"("abc)", false},
      {"truncated literal", "tru", false},
      {"truncated fraction", "1.", false},
      {"truncated exponent", "1e", false},
      {"truncated escape", R"("ab\)", false},
      {"truncated unicode escape", R"("\u12)", false},
      {"unclosed nesting", "[[[[1]]]", false},
      {"truncated deep value", R"({"a":[1,{"b":"x)", false},
      {"open brace only", "{", false},
      // Nothing at all.
      {"empty input", "", false},
      {"whitespace only", "   ", false},
      {"newline only", "\n\t\r ", false},
      // Trailing bytes after a complete value.
      {"trailing garbage", R"({"a":1} x)", false},
      {"second document", R"({"a":1}{"b":2})", false},
      {"number then junk", "123abc", false},
      {"literal then junk", "trueX", false},
      {"trailing comment", R"({"a":1} // note)", false},
      {"embedded comment", "{\"a\":1 /*note*/}", false},
      {"trailing NUL byte", std::string_view{"{\"a\":1}\0", 8}, false},
      // Malformed JSON the read itself rejects.
      {"array trailing comma", "[1,2,]", false},
      {"object trailing comma", R"({"a":1,})", false},
      {"close brace only", "}", false},
      {"colon only", ":", false},
      {"NaN literal", "NaN", false},
      {"leading zero", "01", false},
      {"leading plus", "+1", false},
      {"single quoted string", "'a'", false},
      {"unescaped control character", std::string_view{"\"a\x01\x62\"", 5}, false},
      // Invalid Unicode, in a value or a key.
      {"invalid UTF-8 in a string", "\"\xff\xfe\"", false},
      {"invalid UTF-8 in a key", "{\"\xff\":1}", false},
      {"lone surrogate escape", R"("\ud800")", false},
      // Complete documents, including the ones that differ from the above by one byte.
      {"complete object", R"({"a":1})", true},
      {"complete nested object", R"({"error":{"type":"not_found_error"}})", true},
      {"complete array", "[1,2]", true},
      {"closed nesting", "[[[[1]]]]", true},
      {"trailing space", R"({"a":1} )", true},
      {"trailing newline", "{\"a\":1}\n", true},
      {"leading whitespace", "  {\"a\":1}", true},
      {"bare number", "123", true},
      {"bare number with space", "123 ", true},
      {"bare true", "true", true},
      {"bare null", "null", true},
      {"bare string", R"("abc")", true},
      {"empty object", "{}", true},
      {"empty array", "[]", true},
  };

  for (const auto& adversarial : cases) {
    INFO(adversarial.name);
    CHECK(codec_accepts(adversarial.text) == adversarial.accepted);
  }
}

TEST_CASE("the JSON codec bounds malformed non-null-terminated input") {
  // A lone brace in a buffer with no terminator after it: the read must stop at
  // the view's end rather than look for a NUL.
  constexpr std::array input{'{'};
  const auto text = std::string_view{input.data(), input.size()};
  CHECK_FALSE(text_codec::validate_json(text));
  CHECK_FALSE(codec_accepts(text));
}

TEST_CASE("the JSON codec still collapses duplicate object keys") {
  // docs/architecture.md: "Canonical parsing collapses duplicate object keys before
  // dispatch." The last occurrence wins, at every nesting level.
  auto document = JsonView::parse(scrydantic::Json{.text = R"({"a":1,"a":2})"});
  REQUIRE(document);
  REQUIRE(document->size() == 1);
  CHECK(document->find("a")->unsigned_integer() == 2U);

  auto nested = JsonView::parse(scrydantic::Json{.text = R"({"o":{"k":1,"k":2}})"});
  REQUIRE(nested);
  REQUIRE(nested->find("o")->size() == 1);
  CHECK(nested->find("o")->find("k")->unsigned_integer() == 2U);
}

TEST_CASE("the JSON codec and JsonView write the kernel's canonical text") {
  // One input per canonical-form rule; codec_accepts compares every entry point's
  // output with the document writer's.
  for (const std::string_view input : {
           R"( { "b" : 1 , "a" : [ 2 , { "d" : 3 , "c" : 4 } ] , "a" : null } )",
           "[1.0,-0,1e2,0.00001,1.5e16,18446744073709551615,-9223372036854775808]",
           R"({"\u0041\u00e9\ud83d\ude00":"\/\u001f\b\t"})",
           R"("\u0000")",
           "true",
       }) {
    INFO(input);
    CHECK(codec_accepts(input));
  }
}
