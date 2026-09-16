// What the single-pass JSON codec accepts, pinned against the two-pass validator it
// replaced. src/core/json_codec.cpp used to skip every document once with
// validate_skipped and validate_trailing_whitespace before reading it, purely to
// reject truncation, trailing garbage, and a second document. It now reads once and
// checks Glaze's own completion counter instead, so the whole acceptance boundary is
// asserted here: a table of adversarial documents, and a differential replay of the
// fuzz corpora against the old two-pass path kept below as an oracle.

#include "core/json_codec.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <glaze/glaze.hpp>
#include <iterator>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace {

using scry::JsonView;

// The replaced implementation, verbatim, as the differential oracle.
constexpr glz::opts two_pass_read_options{.null_terminated = false};
struct TwoPassValidateOptions : glz::opts {
  bool validate_skipped = true;
  bool validate_trailing_whitespace = true;
};
constexpr TwoPassValidateOptions two_pass_validate_options{{.null_terminated = false}};

[[nodiscard]] bool two_pass_accepts(const std::string_view input) {
  glz::skip skipped{};
  glz::context context{};
  if (glz::read<two_pass_validate_options>(skipped, input, context)) {
    return false;
  }
  scry::detail::JsonValue value{};
  return !glz::read<two_pass_read_options>(value, input);
}

[[nodiscard]] bool codec_accepts(const std::string_view input) {
  const auto parsed = scry::detail::parse_json(
      input, scry::ErrorCategory::invalid_argument, "JSON text is not valid");
  const auto viewed = JsonView::parse(scry::Json{.text = std::string{input}});
  // The public view and the private codec are one parser; a disagreement is a bug
  // whichever way it points.
  REQUIRE(parsed.has_value() == viewed.has_value());
  if (!parsed) {
    CHECK(parsed.error().category == scry::ErrorCategory::invalid_argument);
    CHECK(parsed.error().message == "JSON text is not valid");
  }
  return parsed.has_value();
}

struct Adversarial {
  std::string_view name{};
  std::string_view text{};
  bool accepted{};
};

[[nodiscard]] std::vector<std::filesystem::path> corpus_files() {
  std::vector<std::filesystem::path> files{};
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator{SCRY_FUZZ_CORPUS_DIR}) {
    if (entry.is_regular_file()) {
      files.push_back(entry.path());
    }
  }
  return files;
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
  std::ifstream input{path, std::ios::binary};
  REQUIRE(input.good());
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE("the JSON codec rejects every shape of incomplete document") {
  // Glaze reports a value that ended with a non-NUL-terminated buffer as the
  // non-error code end_reached, and the variant reader behind glz::generic clears
  // that code before the top level can settle it against the nesting depth. Each
  // rejection below names the byte pattern that would otherwise read as a whole
  // document.
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
      // Invalid UTF-8 was rejected before and stays rejected.
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
    // The two-pass validator is the contract; the single pass must not drift from it.
    CHECK(two_pass_accepts(adversarial.text) == adversarial.accepted);
  }
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

TEST_CASE("the JSON codec matches the two-pass validator on every fuzz corpus byte") {
  // The fuzz targets need Clang, so the corpora are replayed here instead: every
  // seed and every prefix of every seed, which is exactly the truncation the single
  // pass had to learn to reject. Acceptance and the re-serialized document must both
  // match what the two-pass path produced.
  const auto files = corpus_files();
  REQUIRE_FALSE(files.empty());

  std::size_t checked = 0;
  for (const auto& path : files) {
    INFO(path.string());
    const auto text = read_file(path);
    for (std::size_t length = 0; length <= text.size(); ++length) {
      const auto prefix = std::string_view{text}.substr(0, length);
      INFO("prefix length " << length);
      const bool expected = two_pass_accepts(prefix);
      REQUIRE(codec_accepts(prefix) == expected);
      if (expected) {
        scry::detail::JsonValue oracle{};
        REQUIRE_FALSE(glz::read<two_pass_read_options>(oracle, prefix));
        const auto parsed = scry::detail::parse_json(
            prefix, scry::ErrorCategory::invalid_argument, "JSON text is not valid");
        REQUIRE(parsed);
        REQUIRE(glz::write_json(*parsed) == glz::write_json(oracle));
      }
      ++checked;
    }
  }
  CHECK(checked > 0);
}
