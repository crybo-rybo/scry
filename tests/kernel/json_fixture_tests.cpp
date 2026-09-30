// Golden fixtures for Scry's JSON layer (kernel/json/document.hpp). Every
// expectation in tests/fixtures/json/goldens.tar.xz was produced by the JSON codec the
// layer replaced, with the layer's two documented departures applied, and checked
// against the layer when it was written (tests/fixtures/json/README.md). They
// keep the parser's acceptance boundary and the canonical writer's bytes fixed.

#include "kernel/json/document.hpp"
#include "support/json_fixtures.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <filesystem>
#include <string_view>

namespace {

namespace json = scry::detail::json;
namespace fs = std::filesystem;
using namespace scry::test::json_fixtures;

} // namespace

TEST_CASE("the JSON layer matches every golden fixture") {
  const auto fixtures = all_fixtures(SCRY_JSON_FIXTURE_DIR);
  REQUIRE(fixtures.size() > 1000);
  for (const auto& fixture : fixtures) {
    INFO(fixture.location);
    const auto parsed = json::parse(fixture.input);
    CHECK(json::validate(fixture.input) == parsed.has_value());
    REQUIRE(parsed.has_value() == fixture.output.has_value());
    if (!parsed) {
      continue;
    }
    const auto written = json::write(*parsed);
    CHECK(written == *fixture.output);
    // Canonical text is its own canonical form.
    const auto reparsed = json::parse(*fixture.output);
    REQUIRE(reparsed);
    CHECK(json::write(*reparsed) == *fixture.output);
  }
}

TEST_CASE(
    "the JSON layer accepts exactly the recorded prefixes of every fuzz corpus file") {
  // Truncation is the boundary a single-pass parser most easily gets wrong, so
  // every prefix of every checked-in corpus file is replayed: the lengths the
  // fixture lists must parse and every other length must not.
  const auto accepted = accepted_prefixes(SCRY_JSON_FIXTURE_DIR);
  const fs::path corpus_root{SCRY_FUZZ_CORPUS_DIR};
  const auto files = files_under(corpus_root, "");
  REQUIRE(files.size() > 100);
  std::size_t prefixes = 0;
  for (const auto& path : files) {
    const auto name = path.lexically_relative(corpus_root).generic_string();
    INFO(name);
    const auto found = accepted.find(name);
    REQUIRE(found != accepted.end());
    const auto text = read_file(path);
    for (std::size_t length = 0; length <= text.size(); ++length) {
      const auto prefix = std::string_view{text}.substr(0, length);
      INFO("prefix length " << length);
      const bool expected = contains(found->second, length);
      REQUIRE(json::validate(prefix) == expected);
      REQUIRE(json::parse(prefix).has_value() == expected);
      ++prefixes;
    }
  }
  CHECK(prefixes > 10000);
}
