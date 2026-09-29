// Golden fixtures for Scry's JSON layer (kernel/json/document.hpp). Every
// expectation under tests/fixtures/json/ was produced by the Glaze-backed codec
// the layer replaces, with the layer's two documented departures applied
// (tests/fixtures/json/README.md). They are what keeps the parser's acceptance
// boundary and the canonical writer's bytes fixed once Glaze is gone.

#include "kernel/json/document.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace json = scry::detail::json;
namespace fs = std::filesystem;

struct Fixture {
  std::string location{};
  std::string input{};
  // Empty when the input must be rejected.
  std::optional<std::string> output{};
};

[[nodiscard]] std::string read_file(const fs::path& path) {
  std::ifstream stream{path, std::ios::binary};
  REQUIRE(stream.good());
  return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::vector<std::string> read_lines(const fs::path& path) {
  std::vector<std::string> lines{};
  std::ifstream stream{path, std::ios::binary};
  REQUIRE(stream.good());
  for (std::string line{}; std::getline(stream, line);) {
    lines.push_back(std::move(line));
  }
  return lines;
}

// %HH is one byte; every other character stands for itself.
[[nodiscard]] std::string unescape(const std::string_view text) {
  std::string bytes{};
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] != '%') {
      bytes.push_back(text[index]);
      continue;
    }
    REQUIRE(index + 2 < text.size());
    unsigned int byte = 0;
    const auto* digits = text.data() + index + 1;
    REQUIRE(std::from_chars(digits, digits + 2, byte, 16).ptr == digits + 2);
    bytes.push_back(static_cast<char>(byte));
    index += 2;
  }
  return bytes;
}

[[nodiscard]] std::vector<fs::path> files_under(const fs::path& directory,
                                                const std::string_view extension) {
  std::vector<fs::path> files{};
  for (const auto& entry : fs::recursive_directory_iterator{directory}) {
    if (entry.is_regular_file() &&
        (extension.empty() || entry.path().extension() == extension)) {
      files.push_back(entry.path());
    }
  }
  std::ranges::sort(files);
  return files;
}

// The text after a line's marker: "< text", or a bare "<" for empty text.
[[nodiscard]] std::string marked_text(const std::string_view line, const char marker) {
  REQUIRE(!line.empty());
  REQUIRE(line.front() == marker);
  if (line.size() == 1) {
    return {};
  }
  REQUIRE(line[1] == ' ');
  return unescape(line.substr(2));
}

// A case is "< input" followed by "> output" or "x" for a rejection; "#" lines
// and blank lines separate cases.
[[nodiscard]] std::vector<Fixture> read_fixtures(const fs::path& path) {
  std::vector<Fixture> fixtures{};
  const auto lines = read_lines(path);
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const std::string_view line = lines[index];
    if (line.empty() || line.starts_with('#')) {
      continue;
    }
    const auto location = path.filename().string() + ":" + std::to_string(index + 1);
    INFO(location);
    REQUIRE(index + 1 < lines.size());
    const std::string_view expectation = lines[++index];
    Fixture fixture{.location = location, .input = marked_text(line, '<')};
    if (expectation != "x") {
      fixture.output = marked_text(expectation, '>');
    }
    fixtures.push_back(std::move(fixture));
  }
  return fixtures;
}

[[nodiscard]] std::vector<Fixture> all_fixtures() {
  std::vector<Fixture> fixtures{};
  for (const auto& path : files_under(SCRY_JSON_FIXTURE_DIR, ".txt")) {
    auto more = read_fixtures(path);
    fixtures.insert(fixtures.end(), std::make_move_iterator(more.begin()),
                    std::make_move_iterator(more.end()));
  }
  return fixtures;
}

// "<corpus path> <ranges>": the prefix lengths of that corpus file the parser
// accepts, as comma-separated lengths and inclusive "low-high" ranges, or
// "none".
[[nodiscard]] std::map<std::string, std::vector<std::pair<std::size_t, std::size_t>>>
accepted_prefixes() {
  std::map<std::string, std::vector<std::pair<std::size_t, std::size_t>>> result{};
  const auto path = fs::path{SCRY_JSON_FIXTURE_DIR} / "corpus_prefixes.list";
  for (const auto& line : read_lines(path)) {
    if (line.empty() || line.starts_with('#')) {
      continue;
    }
    const auto space = line.find(' ');
    REQUIRE(space != std::string::npos);
    auto& ranges = result[line.substr(0, space)];
    std::string_view rest = std::string_view{line}.substr(space + 1);
    if (rest == "none") {
      continue;
    }
    while (!rest.empty()) {
      const auto comma = rest.find(',');
      const auto item = rest.substr(0, comma);
      rest =
          comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
      const auto dash = item.find('-');
      std::size_t low = 0;
      std::size_t high = 0;
      const auto low_text = item.substr(0, dash);
      REQUIRE(
          std::from_chars(low_text.data(), low_text.data() + low_text.size(), low).ec ==
          std::errc{});
      high = low;
      if (dash != std::string_view::npos) {
        const auto high_text = item.substr(dash + 1);
        REQUIRE(
            std::from_chars(high_text.data(), high_text.data() + high_text.size(), high)
                .ec == std::errc{});
      }
      ranges.emplace_back(low, high);
    }
  }
  return result;
}

[[nodiscard]] bool
contains(const std::vector<std::pair<std::size_t, std::size_t>>& ranges,
         const std::size_t length) {
  return std::ranges::any_of(ranges, [length](const auto& range) {
    return length >= range.first && length <= range.second;
  });
}

} // namespace

TEST_CASE("the JSON layer matches every golden fixture") {
  const auto fixtures = all_fixtures();
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
  const auto accepted = accepted_prefixes();
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
