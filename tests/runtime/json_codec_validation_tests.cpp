// What the JSON codec accepts, seen from the rest of src/: the text-level entry
// points in kernel/json/codec.hpp and the public JsonView are one parser, so each
// must accept exactly what the others do. The oracle is the golden fixtures
// under tests/fixtures/json/, which pin the acceptance boundary and the canonical
// bytes (tests/kernel/json_fixture_tests.cpp holds the kernel layer itself to
// them), plus a table of adversarial documents named by shape.

#include "kernel/json/codec.hpp"
#include "support/json_fixtures.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <optional>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace {

using scry::JsonView;
namespace fixtures = scry::test::json_fixtures;

constexpr auto category = scry::ErrorCategory::invalid_argument;
constexpr std::string_view failure = "JSON text is not valid";

// Every entry point's verdict on `input`; they must agree, whichever way.
[[nodiscard]] bool codec_accepts(const std::string_view input) {
  const bool validated =
      scry::detail::validate_json(input, category, failure).has_value();
  const auto canonical = scry::detail::canonicalize_json(
      scry::Json{.text = std::string{input}}, category, failure);
  const auto viewed = JsonView::parse(scry::Json{.text = std::string{input}});
  REQUIRE(canonical.has_value() == validated);
  REQUIRE(viewed.has_value() == validated);
  if (!viewed) {
    CHECK(viewed.error().category == scry::ErrorCategory::invalid_argument);
    CHECK(viewed.error().message == "JSON text is not valid");
    return false;
  }
  // The view and the codec write one canonical form.
  CHECK(viewed->to_json().text == canonical->text);
  return true;
}

// The canonical text of an accepted input, through the codec.
[[nodiscard]] std::optional<std::string> canonical_text(const std::string_view input) {
  auto canonical = scry::detail::canonicalize_json(
      scry::Json{.text = std::string{input}}, category, failure);
  return canonical ? std::optional<std::string>{std::move(canonical->text)}
                   : std::nullopt;
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
  CHECK_FALSE(scry::detail::validate_json(text, scry::ErrorCategory::protocol,
                                          "invalid test JSON"));
  CHECK_FALSE(codec_accepts(text));
}

TEST_CASE("the JSON codec still collapses duplicate object keys") {
  // docs/architecture.md: "Canonical parsing collapses duplicate object keys before
  // dispatch." The last occurrence wins, at every nesting level.
  auto document = JsonView::parse(scry::Json{.text = R"({"a":1,"a":2})"});
  REQUIRE(document);
  REQUIRE(document->size() == 1);
  CHECK(document->find("a")->unsigned_integer() == 2U);

  auto nested = JsonView::parse(scry::Json{.text = R"({"o":{"k":1,"k":2}})"});
  REQUIRE(nested);
  REQUIRE(nested->find("o")->size() == 1);
  CHECK(nested->find("o")->find("k")->unsigned_integer() == 2U);
}

TEST_CASE("the JSON codec and JsonView match every golden fixture") {
  const auto cases = fixtures::all_fixtures(SCRY_JSON_FIXTURE_DIR);
  REQUIRE(cases.size() > 1000);
  for (const auto& fixture : cases) {
    INFO(fixture.location);
    REQUIRE(codec_accepts(fixture.input) == fixture.output.has_value());
    if (!fixture.output) {
      continue;
    }
    CHECK(canonical_text(fixture.input) == fixture.output);
    // The object variants accept exactly the documents whose root is an object.
    const bool object = fixture.output->starts_with('{');
    CHECK(scry::detail::validate_json_object(fixture.input, category, failure)
              .has_value() == object);
    CHECK(scry::detail::canonicalize_json_object(scry::Json{.text = fixture.input},
                                                 category, failure)
              .has_value() == object);
  }
}

TEST_CASE("the JSON codec accepts exactly the recorded prefixes of every fuzz corpus") {
  // The single pass has to reject truncation wherever it falls, so every prefix of
  // every checked-in corpus file goes through every entry point.
  const auto accepted = fixtures::accepted_prefixes(SCRY_JSON_FIXTURE_DIR);
  const std::filesystem::path corpus_root{SCRY_FUZZ_CORPUS_DIR};
  std::size_t checked = 0;
  for (const auto& path : fixtures::files_under(corpus_root, "")) {
    const auto name = path.lexically_relative(corpus_root).generic_string();
    INFO(name);
    const auto found = accepted.find(name);
    REQUIRE(found != accepted.end());
    const auto text = fixtures::read_file(path);
    for (std::size_t length = 0; length <= text.size(); ++length) {
      INFO("prefix length " << length);
      REQUIRE(codec_accepts(std::string_view{text}.substr(0, length)) ==
              fixtures::contains(found->second, length));
      ++checked;
    }
  }
  CHECK(checked > 10000);
}

TEST_CASE("the JSON codec quotes and spells numbers canonically") {
  CHECK(scry::detail::make_json_error_object("a\x1f\"b").text ==
        "{\"error\":\"a\\u001F\\\"b\"}");
  CHECK(scry::detail::canonical_json_number(0.1).text == "0.1");
  CHECK(scry::detail::canonical_json_number(1e20).text == "1E20");
  CHECK(scry::detail::canonical_json_number(-0.0).text == "-0");
  CHECK(scry::detail::canonical_json_number(2.0).text == "2");
  CHECK(scry::detail::canonical_json_number(std::numeric_limits<double>::infinity())
            .text == "null");
  CHECK(scry::detail::canonical_json_number(std::numeric_limits<double>::quiet_NaN())
            .text == "null");
}
