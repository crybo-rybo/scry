// Golden fixtures for Scry's JSON layer (kernel/json/document.hpp), unpacked by
// the build from tests/fixtures/json/goldens.tar.xz, whose README gives their
// provenance and format. They keep the parser's acceptance boundary and the
// canonical writer's bytes fixed.

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

[[nodiscard]] std::size_t parse_size(const std::string_view text, const int base) {
  std::size_t value = 0;
  const auto* end = text.data() + text.size();
  REQUIRE(std::from_chars(text.data(), end, value, base).ptr == end);
  return value;
}

// The text after a line's marker ("< text", or a bare "<" for empty text), where
// %HH is one byte and every other character stands for itself.
[[nodiscard]] std::string marked_text(const std::string_view line, const char marker) {
  REQUIRE(!line.empty());
  REQUIRE(line.front() == marker);
  REQUIRE((line.size() == 1 || line[1] == ' '));
  std::string bytes{};
  for (std::size_t index = 2; index < line.size(); ++index) {
    if (line[index] != '%') {
      bytes.push_back(line[index]);
      continue;
    }
    REQUIRE(index + 2 < line.size());
    bytes.push_back(static_cast<char>(parse_size(line.substr(index + 1, 2), 16)));
    index += 2;
  }
  return bytes;
}

// A case is "< input" followed by "> output" or "x" for a rejection; "#" lines
// and blank lines separate cases.
[[nodiscard]] std::vector<Fixture> all_fixtures(const fs::path& directory) {
  std::vector<Fixture> fixtures{};
  for (const auto& path : files_under(directory, ".txt")) {
    const auto lines = read_lines(path);
    for (std::size_t index = 0; index < lines.size(); ++index) {
      const std::string_view line = lines[index];
      if (line.empty() || line.starts_with('#')) {
        continue;
      }
      auto location = path.filename().string() + ":" + std::to_string(index + 1);
      INFO(location);
      REQUIRE(index + 1 < lines.size());
      const std::string_view expectation = lines[++index];
      Fixture fixture{.location = std::move(location), .input = marked_text(line, '<')};
      if (expectation != "x") {
        fixture.output = marked_text(expectation, '>');
      }
      fixtures.push_back(std::move(fixture));
    }
  }
  return fixtures;
}

using Ranges = std::vector<std::pair<std::size_t, std::size_t>>;

// "<corpus path> <ranges>": the prefix lengths of that corpus file the parser
// accepts, as comma-separated lengths and inclusive "low-high" ranges, or "none".
[[nodiscard]] std::map<std::string, Ranges> accepted_prefixes(const fs::path& path) {
  std::map<std::string, Ranges> result{};
  for (const auto& line : read_lines(path)) {
    if (line.empty() || line.starts_with('#')) {
      continue;
    }
    const auto space = line.find(' ');
    REQUIRE(space != std::string::npos);
    auto& ranges = result[line.substr(0, space)];
    std::string_view rest = std::string_view{line}.substr(space + 1);
    while (rest != "none" && !rest.empty()) {
      const auto item = rest.substr(0, rest.find(','));
      rest.remove_prefix(std::min(rest.size(), item.size() + 1));
      const auto dash = item.find('-');
      const auto low = parse_size(item.substr(0, dash), 10);
      ranges.emplace_back(low, dash == std::string_view::npos
                                   ? low
                                   : parse_size(item.substr(dash + 1), 10));
    }
  }
  return result;
}

[[nodiscard]] bool contains(const Ranges& ranges, const std::size_t length) {
  return std::ranges::any_of(ranges, [length](const auto& range) {
    return length >= range.first && length <= range.second;
  });
}

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
  const auto accepted =
      accepted_prefixes(fs::path{SCRY_JSON_FIXTURE_DIR} / "corpus_prefixes.list");
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
